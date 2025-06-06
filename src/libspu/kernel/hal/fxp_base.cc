// Copyright 2021 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "libspu/kernel/hal/fxp_base.h"

#include <cmath>

#include "libspu/core/prelude.h"
#include "libspu/core/trace.h"
#include "libspu/kernel/hal/constants.h"
#include "libspu/kernel/hal/fxp_cleartext.h"
#include "libspu/kernel/hal/ring.h"

namespace spu::kernel::hal {
namespace detail {

// Calc:
//   y = c0 + x*c1 + x^2*c2 + x^3*c3 + ... + x^n*c[n]
Value polynomial(SPUContext* ctx, const Value& x,
                 absl::Span<Value const> coeffs, SignType sign_x,
                 SignType sign_ret) {
  SPU_TRACE_HAL_DISP(ctx, x);
  SPU_ENFORCE(x.isFxp());
  SPU_ENFORCE(!coeffs.empty());

  if (coeffs.size() == 1U || x.numel() == 0) {
    return coeffs[0];
  }
  // Use a parallel circuit to calculate x, x^2, x^3, ..., x^n.
  // The general log(n) algorithm
  // algorithm:
  //  Step 0. x
  //  Step 1. x, x2
  //  Step 2. x, x2, x3, x4
  //  ...
  std::vector<spu::Value> x_prefix(1, x);
  size_t degree = coeffs.size() - 1;
  for (int64_t i = 0; i < Log2Ceil(degree); ++i) {
    size_t x_size = std::min(x_prefix.size(), degree - x_prefix.size());
    std::vector<spu::Value> x_pow(x_size, x_prefix.back());
    // TODO: this can be further optimized to use sign hint
    vmap(x_prefix.begin(), x_prefix.begin() + x_size, x_pow.begin(),
         x_pow.end(), std::back_inserter(x_prefix),
         [ctx, sign_x](const Value& a, const Value& b) {
           return f_mul(ctx, a, b, sign_x);
         });
  }

  Value res = _mul(ctx, constant(ctx, 1.0F, x.dtype(), x.shape()), coeffs[0]);

  const auto fbits = ctx->getFxpBits();
  for (size_t i = 1; i < coeffs.size(); i++) {
    res = _add(ctx, res, _mul(ctx, x_prefix[i - 1], coeffs[i]));
  }

  return _trunc(ctx, res, fbits, sign_ret).setDtype(x.dtype());
}

Value polynomial(SPUContext* ctx, const Value& x,
                 absl::Span<float const> coeffs, SignType sign_x,
                 SignType sign_ret) {
  std::vector<Value> cs;
  cs.reserve(coeffs.size());
  for (const auto& c : coeffs) {
    cs.push_back(constant(ctx, c, x.dtype(), x.shape()));
  }
  return polynomial(ctx, x, cs, sign_x, sign_ret);
}

Value highestOneBit(SPUContext* ctx, const Value& x) {
  auto y = _prefix_or(ctx, x);
  auto y1 = _rshift(ctx, y, {1});
  return _xor(ctx, y, y1);
}

// FIXME:
// Use range propagation instead of directly set.
// or expose bit_decompose as mpc level api.
void hintNumberOfBits(const Value& a, size_t nbits) {
  if (a.storage_type().isa<BShare>()) {
    const_cast<Type&>(a.storage_type()).as<BShare>()->setNbits(nbits);
  }
}

Value maskNumberOfBits(SPUContext* ctx, const Value& in, size_t nbits) {
  auto k1 = constant(ctx, static_cast<int64_t>(1), spu::DT_I64, in.shape());
  auto mask = _sub(ctx, _lshift(ctx, k1, {static_cast<int64_t>(nbits)}), k1);
  auto out = _and(ctx, in, mask).setDtype(in.dtype());
  return out;
}

namespace {
Value reciprocal_goldschmidt_normalized_approx(SPUContext* ctx,
                                               const Value& b_abs,
                                               const Value& factor) {
  // compute normalize x_abs, [0.5, 1)
  auto c = f_mul(ctx, b_abs, factor, SignType::Positive);

  // initial guess:
  //   w = 1/c ≈ 2.9142 - 2c when c >= 0.5 and c < 1
  const auto k2 = _constant(ctx, 2, c.shape());
  const auto k2_9142 = constant(ctx, 2.9142F, b_abs.dtype(), c.shape());
  auto w = f_sub(ctx, k2_9142, _mul(ctx, k2, c).setDtype(b_abs.dtype()));

  // init r=w, e=1-c*w
  const auto& k1_ = constant(ctx, 1.0F, b_abs.dtype(), c.shape());
  auto r = w;
  auto e = f_sub(ctx, k1_, f_mul(ctx, c, w, SignType::Positive));

  size_t num_iters = ctx->config().fxp_div_goldschmidt_iters;
  if (ctx->getFxpBits() >= 30) {
    // default 2 iters of goldschmidt can only get precision about 14 bits.
    // so if fxp>=30, we use 3 iters by default, which get about 28 bits
    // precision.
    num_iters = std::max(num_iters, static_cast<size_t>(3));
  }
  SPU_ENFORCE(num_iters != 0, "fxp_div_goldschmidt_iters should not be {}",
              num_iters);

  // iterate, r=r(1+e), e=e*e
  for (size_t itr = 0; itr < num_iters; itr++) {
    r = f_mul(ctx, r, f_add(ctx, e, k1_), SignType::Positive);
    if (itr + 1 < num_iters) {
      e = f_square(ctx, e);
    }
  }

  return r;
}
}  // namespace

// Reference:
//   Chapter 3.4 Division @ Secure Computation With Fixed Point Number
//   http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.221.1305&rep=rep1&type=pdf
//
// Goldschmidt main idea:
// Target:
//   calculate a/b
//
// Symbols:
//   f: number of fractional bits in fixed point.
//   m: the highest position of bit with value == 1.
//
// Initial guess:
//   let b = c*2^{m} where c = normalize(x), c \in [0.5, 1)
//   let w = 1/c ≈ (2.9142 - 2*c) as the initial guess.
//
// Iteration (reduce error):
//   let r = w, denotes result
//   let e = 1-c*w, denotes error
//   for _ in iters:
//     r = r(1 + e)
//     e = e * e
//
//   return r * a * 2^{-m}
//
// Precision is decided by magic number, i.e 2.9142 and f.
Value div_goldschmidt_general(SPUContext* ctx, const Value& a, const Value& b,
                              SignType a_sign, SignType b_sign) {
  Value b_abs;
  Value is_negative;
  if (b_sign == SignType::Unknown) {
    // We prefer  b_abs = b < 0 ? -b : b over b_abs = sign(b) * b
    // because MulA1B is a better choice than MulAA for CHEETAH.
    // For ABY3, these two computations give the same cost though.
    is_negative = _msb(ctx, b);
    // insert ``prefer_a'' because the msb bit are used twice.
    is_negative = _prefer_a(ctx, is_negative);
    b_abs = _mux(ctx, is_negative, _negate(ctx, b), b).setDtype(b.dtype());
  } else if (b_sign == SignType::Positive) {
    is_negative = _constant(ctx, 0, b.shape());
    b_abs = b;
  } else {
    // b is negative
    is_negative = _constant(ctx, static_cast<uint128_t>(1), b.shape());
    b_abs = _negate(ctx, b).setDtype(b.dtype());
  }

  auto b_msb = detail::highestOneBit(ctx, b_abs);

  // factor = 2^{f-m} = 2^{-m} * 2^f, the fixed point repr of 2^{-m}
  const size_t num_fxp_bits = ctx->getFxpBits();
  auto factor = _bitrev(ctx, b_msb, 0, 2 * num_fxp_bits).setDtype(b.dtype());

  factor = maskNumberOfBits(ctx, factor, 2 * num_fxp_bits);
  // also, we use factor twice
  factor = _prefer_a(ctx, factor);

  // compute approximation of normalize b_abs
  auto r = reciprocal_goldschmidt_normalized_approx(ctx, b_abs, factor);

  // r from goldschmidt iteration is always positive
  // so sign(r*a) = sign(a)
  r = f_mul(ctx, r, a, a_sign);
  // also, sign(r*factor) = sign(r)
  r = f_mul(ctx, r, factor, a_sign);

  return _mux(ctx, is_negative, _negate(ctx, r), r).setDtype(a.dtype());
}

Value div_goldschmidt(SPUContext* ctx, const Value& a, const Value& b) {
  SPU_TRACE_HAL_DISP(ctx, a, b);

  return div_goldschmidt_general(ctx, a, b);
}

Value reciprocal_goldschmidt_positive(SPUContext* ctx, const Value& b_abs) {
  SPU_TRACE_HAL_DISP(ctx, b_abs);

  auto b_msb = detail::highestOneBit(ctx, b_abs);

  // factor = 2^{f-m} = 2^{-m} * 2^f, the fixed point repr of 2^{-m}
  const size_t num_fxp_bits = ctx->getFxpBits();
  auto factor =
      _bitrev(ctx, b_msb, 0, 2 * num_fxp_bits).setDtype(b_abs.dtype());
  factor = maskNumberOfBits(ctx, factor, 2 * num_fxp_bits);
  // also, we use factor twice
  factor = _prefer_a(ctx, factor);

  // compute approximation of normalize b_abs
  auto r = reciprocal_goldschmidt_normalized_approx(ctx, b_abs, factor);

  return f_mul(ctx, r, factor, SignType::Positive);
}

// NOTE(junfeng): we have a separate reciprocal_goldschmidt is to avoid
// unnecessary f_mul for y initiation in div_goldschmidt.
Value reciprocal_goldschmidt(SPUContext* ctx, const Value& b) {
  SPU_TRACE_HAL_DISP(ctx, b);

  // We prefer  b_abs = b < 0 ? -b : b over b_abs = sign(b) * b
  // because MulA1B is a better choice than MulAA for CHEETAH.
  // For ABY3, these two computations give the same cost though.
  auto is_negative = _msb(ctx, b);
  // insert ``prefer_a'' because the msb bit are used twice.
  is_negative = _prefer_a(ctx, is_negative);
  auto b_abs = _mux(ctx, is_negative, _negate(ctx, b), b).setDtype(b.dtype());

  auto b_msb = detail::highestOneBit(ctx, b_abs);

  // factor = 2^{f-m} = 2^{-m} * 2^f, the fixed point repr of 2^{-m}
  const size_t num_fxp_bits = ctx->getFxpBits();
  auto factor = _bitrev(ctx, b_msb, 0, 2 * num_fxp_bits).setDtype(b.dtype());
  factor = maskNumberOfBits(ctx, factor, 2 * num_fxp_bits);
  // also, we use factor twice
  factor = _prefer_a(ctx, factor);

  // compute approximation of normalize b_abs
  auto r = reciprocal_goldschmidt_normalized_approx(ctx, b_abs, factor);
  r = f_mul(ctx, r, factor, SignType::Positive);

  return _mux(ctx, is_negative, _negate(ctx, r), r).setDtype(b.dtype());
}

}  // namespace detail

Value f_negate(SPUContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  SPU_ENFORCE(x.isFxp());
  return _negate(ctx, x).setDtype(x.dtype());
}

Value f_abs(SPUContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  SPU_ENFORCE(x.isFxp());
  const Value sign = _sign(ctx, x);

  return _mul(ctx, sign, x).setDtype(x.dtype());
}

Value f_reciprocal(SPUContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  SPU_ENFORCE(x.isFxp());
  if (x.isPublic()) {
    return f_reciprocal_p(ctx, x);
  }

  return detail::reciprocal_goldschmidt(ctx, x);
}

Value f_add(SPUContext* ctx, const Value& x, const Value& y) {
  SPU_TRACE_HAL_LEAF(ctx, x, y);

  SPU_ENFORCE(x.isFxp() && y.isFxp() && x.dtype() == y.dtype());

  return _add(ctx, x, y).setDtype(x.dtype());
}

Value f_sub(SPUContext* ctx, const Value& x, const Value& y) {
  SPU_TRACE_HAL_LEAF(ctx, x, y);

  SPU_ENFORCE(x.isFxp() && y.isFxp() && x.dtype() == y.dtype());

  return f_add(ctx, x, f_negate(ctx, y));
}

Value f_mul(SPUContext* ctx, const Value& x, const Value& y, SignType sign) {
  SPU_TRACE_HAL_LEAF(ctx, x, y);

  SPU_ENFORCE(x.isFxp() && y.isFxp() && x.dtype() == y.dtype());

  return _trunc(ctx, _mul(ctx, x, y), ctx->getFxpBits(), sign)
      .setDtype(x.dtype());
}

Value f_mmul(SPUContext* ctx, const Value& x, const Value& y) {
  SPU_TRACE_HAL_LEAF(ctx, x, y);

  SPU_ENFORCE(x.isFxp() && y.isFxp() && x.dtype() == y.dtype());

  return _trunc(ctx, _mmul(ctx, x, y)).setDtype(x.dtype());
}

Value f_conv2d(SPUContext* ctx, const Value& x, const Value& y,
               const Strides& window_strides) {
  SPU_TRACE_HAL_LEAF(ctx, x, y, window_strides);

  SPU_ENFORCE(x.isFxp() && y.isFxp() && x.dtype() == y.dtype());

  return _trunc(ctx, _conv2d(ctx, x, y, window_strides)).setDtype(x.dtype());
}

Value f_tensordot(SPUContext* ctx, const Value& x, const Value& y,
                  const Index& ix, const Index& iy) {
  SPU_TRACE_HAL_LEAF(ctx, x, y, ix, iy);

  SPU_ENFORCE(x.isFxp() && y.isFxp() && x.dtype() == y.dtype());

  return _trunc(ctx, _tensordot(ctx, x, y, ix, iy)).setDtype(x.dtype());
}

Value f_div(SPUContext* ctx, const Value& x, const Value& y) {
  SPU_TRACE_HAL_LEAF(ctx, x, y);

  SPU_ENFORCE(x.isFxp() && y.isFxp() && x.dtype() == y.dtype());

  if (x.isPublic() && y.isPublic()) {
    return f_div_p(ctx, x, y);
  }

  return detail::div_goldschmidt(ctx, x, y);
}

Value f_equal(SPUContext* ctx, const Value& x, const Value& y) {
  SPU_TRACE_HAL_LEAF(ctx, x, y);

  SPU_ENFORCE(x.isFxp() && y.isFxp() && x.dtype() == y.dtype());

  return _equal(ctx, x, y).setDtype(DT_I1);
}

Value f_less(SPUContext* ctx, const Value& x, const Value& y) {
  SPU_TRACE_HAL_LEAF(ctx, x, y);

  SPU_ENFORCE(x.isFxp() && y.isFxp() && x.dtype() == y.dtype());

  return _less(ctx, x, y).setDtype(DT_I1);
}

Value f_square(SPUContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  SPU_ENFORCE(x.isFxp(), "{}", x);
  return _trunc(ctx, _square(ctx, x), ctx->getFxpBits(), SignType::Positive)
      .setDtype(x.dtype());
}

Value f_floor(SPUContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  SPU_ENFORCE(x.isFxp());

  const int64_t fbits = ctx->getFxpBits();
  return _lshift(ctx, _arshift(ctx, x, {fbits}), {fbits}).setDtype(x.dtype());
}

Value f_ceil(SPUContext* ctx, const Value& x) {
  SPU_TRACE_HAL_LEAF(ctx, x);

  SPU_ENFORCE(x.isFxp());

  // ceil(x) = floor(x + 1.0 - epsilon)
  const auto k1 = constant(ctx, 1.0F, x.dtype(), x.shape());
  return f_floor(
      ctx, f_add(ctx, x, f_sub(ctx, k1, epsilon(ctx, x.dtype(), x.shape()))));
}

}  // namespace spu::kernel::hal
