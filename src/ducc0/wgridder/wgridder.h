/*
 *  This file is part of nifty_gridder.
 *
 *  nifty_gridder is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  nifty_gridder is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with nifty_gridder; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* Copyright (C) 2019-2022 Max-Planck-Society
   Author: Martin Reinecke */

#ifndef DUCC0_WGRIDDER_H
#define DUCC0_WGRIDDER_H

#include <cstring>
#include <complex>
#include <cstdint>
#include <functional>
#include <map>
#include <type_traits>
#include <utility>
#include <mutex>
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <array>
#include <memory>
#if ((!defined(DUCC0_NO_SIMD)) && (defined(__AVX__)||defined(__SSE3__)))
#include <x86intrin.h>
#endif

#include "ducc0/infra/error_handling.h"
#include "ducc0/math/constants.h"
#include "ducc0/fft/fft.h"
#include "ducc0/infra/threading.h"
#include "ducc0/infra/misc_utils.h"
#include "ducc0/infra/useful_macros.h"
#include "ducc0/infra/mav.h"
#include "ducc0/infra/simd.h"
#include "ducc0/infra/timers.h"
#include "ducc0/infra/sycl_utils.h"
#include "ducc0/math/gridding_kernel.h"
#include "ducc0/math/rangeset.h"

namespace ducc0 {

namespace detail_gridder {

using namespace std;
// the next line is necessary to address some sloppy name choices in hipSYCL
using std::min, std::max;

// part of C++20
template<typename T> constexpr T bit_width(T x) noexcept
  {
  T res=0;
  while((x>>res)!=0) ++res;
  return res;
  }

template<typename T> constexpr inline int mysimdlen
  = min<int>(8, native_simd<T>::size());

template<typename T> using mysimd = typename simd_select<T,mysimdlen<T>>::type;

template<typename T> T sqr(T val) { return val*val; }

template<typename T> void quickzero(vmav<T,2> &arr, size_t nthreads)
  {
#if 0
  arr.fill(T(0));
#else
  MR_assert((arr.stride(0)>0) && (arr.stride(1)>0), "bad memory ordering");
  MR_assert(arr.stride(0)>=arr.stride(1), "bad memory ordering");
  size_t s0=arr.shape(0), s1=arr.shape(1);
  execParallel(s0, nthreads, [&](size_t lo, size_t hi)
    {
    if (arr.stride(1)==1)
      {
      if (size_t(arr.stride(0))==arr.shape(1))
        memset(reinterpret_cast<char *>(&arr(lo,0)), 0, sizeof(T)*s1*(hi-lo));
      else
        for (auto i=lo; i<hi; ++i)
          memset(reinterpret_cast<char *>(&arr(i,0)), 0, sizeof(T)*s1);
      }
    else
      for (auto i=lo; i<hi; ++i)
        for (size_t j=0; j<s1; ++j)
          arr(i,j) = T(0);
    });
#endif
  }

template<typename T, typename F> [[gnu::hot]] void expi(vector<complex<T>> &res, vector<T> &buf, F getang)
  {
  using Tsimd = native_simd<T>;
  static constexpr auto vlen = Tsimd::size();
  auto n=res.size();
  for (size_t j=0; j<n; ++j)
    buf[j] = getang(j);
  size_t i=0;
  for (; i+vlen-1<n; i+=vlen)
    {
    auto vang = Tsimd(&buf[i],element_aligned_tag());
    auto vcos = cos(vang);
    auto vsin = sin(vang);
    for (size_t ii=0; ii<vlen; ++ii)
      res[i+ii] = complex<T>(vcos[ii], vsin[ii]);
    }
  for (; i<n; ++i)
    res[i] = complex<T>(cos(buf[i]), sin(buf[i]));
  }

template<typename T> complex<T> hsum_cmplx(mysimd<T> vr, mysimd<T> vi)
  { return complex<T>(reduce(vr, plus<>()), reduce(vi, plus<>())); }

#if (!defined(DUCC0_NO_SIMD))
#if (defined(__AVX__))
#if 1
template<> inline complex<float> hsum_cmplx<float>(mysimd<float> vr, mysimd<float> vi)
  {
  auto t1 = _mm256_hadd_ps(__m256(vr), __m256(vi));
  auto t2 = _mm_hadd_ps(_mm256_extractf128_ps(t1, 0), _mm256_extractf128_ps(t1, 1));
  t2 += _mm_shuffle_ps(t2, t2, _MM_SHUFFLE(1,0,3,2));
  return complex<float>(t2[0], t2[1]);
  }
#else
// this version may be slightly faster, but this needs more benchmarking
template<> inline complex<float> hsum_cmplx<float>(mysimd<float> vr, mysimd<float> vi)
  {
  auto t1 = _mm256_shuffle_ps(vr, vi, _MM_SHUFFLE(0,2,0,2));
  auto t2 = _mm256_shuffle_ps(vr, vi, _MM_SHUFFLE(1,3,1,3));
  auto t3 = _mm256_add_ps(t1,t2);
  t3 = _mm256_shuffle_ps(t3, t3, _MM_SHUFFLE(3,0,2,1));
  auto t4 = _mm_add_ps(_mm256_extractf128_ps(t3, 1), _mm256_castps256_ps128(t3));
  auto t5 = _mm_add_ps(t4, _mm_movehl_ps(t4, t4));
  return complex<float>(t5[0], t5[1]);
  }
#endif
#elif defined(__SSE3__)
template<> inline complex<float> hsum_cmplx<float>(mysimd<float> vr, mysimd<float> vi)
  {
  auto t1 = _mm_hadd_ps(__m128(vr), __m128(vi));
  t1 += _mm_shuffle_ps(t1, t1, _MM_SHUFFLE(2,3,0,1));
  return complex<float>(t1[0], t1[2]);
  }
#endif
#endif

template<size_t ndim> void checkShape
  (const array<size_t, ndim> &shp1, const array<size_t, ndim> &shp2)
  { MR_assert(shp1==shp2, "shape mismatch"); }

//
// Start of real gridder functionality
//

template<typename T> void complex2hartley
  (const cmav<complex<T>, 2> &grid, vmav<T,2> &grid2, size_t nthreads)
  {
  MR_assert(grid.conformable(grid2), "shape mismatch");
  size_t nu=grid.shape(0), nv=grid.shape(1);

  execParallel(nu, nthreads, [&](size_t lo, size_t hi)
    {
    for(auto u=lo, xu=(u==0) ? 0 : nu-u; u<hi; ++u, xu=nu-u)
      for (size_t v=0, xv=0; v<nv; ++v, xv=nv-v)
#ifdef DUCC0_USE_PROPER_HARTLEY_CONVENTION
        grid2(u,v) = T(0.5)*(grid( u, v).real()-grid( u, v).imag()+
                             grid(xu,xv).real()+grid(xu,xv).imag());
#else
        grid2(u,v) = T(0.5)*(grid( u, v).real()+grid( u, v).imag()+
                             grid(xu,xv).real()-grid(xu,xv).imag());
#endif
    });
  }

template<typename T> void hartley2complex
  (const cmav<T,2> &grid, vmav<complex<T>,2> &grid2, size_t nthreads)
  {
  MR_assert(grid.conformable(grid2), "shape mismatch");
  size_t nu=grid.shape(0), nv=grid.shape(1);

  execParallel(nu, nthreads, [&](size_t lo, size_t hi)
    {
    for(size_t u=lo, xu=(u==0) ? 0 : nu-u; u<hi; ++u, xu=nu-u)
      for (size_t v=0, xv=0; v<nv; ++v, xv=nv-v)
#ifdef DUCC0_USE_PROPER_HARTLEY_CONVENTION
        grid2(u,v) = complex<T>(T(.5)*(grid(u,v)+grid(xu,xv)),
                                T(.5)*(grid(xu,xv)-grid(u,v)));
#else
        grid2(u,v) = complex<T>(T(.5)*(grid(u,v)+grid(xu,xv)),
                                T(.5)*(grid(u,v)-grid(xu,xv)));
#endif
    });
  }

template<typename T> void hartley2_2D(vmav<T,2> &arr, size_t vlim,
  bool first_fast, size_t nthreads)
  {
  size_t nu=arr.shape(0), nv=arr.shape(1);
  vfmav<T> farr(arr);
  if (2*vlim<nv)
    {
    if (!first_fast)
      r2r_separable_hartley(farr, farr, {1}, T(1), nthreads);
    auto flo = subarray(farr, {{}, {0,vlim}});
    r2r_separable_hartley(flo, flo, {0}, T(1), nthreads);
    auto fhi = subarray(farr, {{}, {farr.shape(1)-vlim, MAXIDX}});
    r2r_separable_hartley(fhi, fhi, {0}, T(1), nthreads);
    if (first_fast)
      r2r_separable_hartley(farr, farr, {1}, T(1), nthreads);
    }
  else
    r2r_separable_hartley(farr, farr, {0,1}, T(1), nthreads);

  execParallel((nu+1)/2-1, nthreads, [&](size_t lo, size_t hi)
    {
    for(auto i=lo+1; i<hi+1; ++i)
      for(size_t j=1; j<(nv+1)/2; ++j)
        {
        T ll = arr(i   ,j   );
        T hl = arr(nu-i,j   );
        T lh = arr(i   ,nv-j);
        T hh = arr(nu-i,nv-j);
        T v = T(0.5)*(ll+lh+hl+hh);
        arr(i   ,j   ) = v-hh;
        arr(nu-i,j   ) = v-lh;
        arr(i   ,nv-j) = v-hl;
        arr(nu-i,nv-j) = v-ll;
        }
     });
  }

class Uvwidx
  {
  public:
    uint16_t tile_u, tile_v, minplane;

    Uvwidx() {}
    Uvwidx(uint16_t tile_u_, uint16_t tile_v_, uint16_t minplane_)
      : tile_u(tile_u_), tile_v(tile_v_), minplane(minplane_) {}

    uint64_t idx() const
      { return (uint64_t(tile_u)<<32) + (uint64_t(tile_v)<<16) + minplane; }
    bool operator!=(const Uvwidx &other) const
      { return idx()!=other.idx(); }
    bool operator<(const Uvwidx &other) const
      { return idx()<other.idx(); }
  };

class RowchanRange
  {
  public:
    uint32_t row;
    uint16_t ch_begin, ch_end;

    RowchanRange(uint32_t row_, uint16_t ch_begin_, uint16_t ch_end_)
      : row(row_), ch_begin(ch_begin_), ch_end(ch_end_) {}
  };

using VVR = vector<pair<Uvwidx, vector<RowchanRange>>>;

struct UVW
  {
  double u, v, w;
  UVW() {}
  UVW(double u_, double v_, double w_) : u(u_), v(v_), w(w_) {}
  UVW operator* (double fct) const
    { return UVW(u*fct, v*fct, w*fct); }
  void Flip() { u=-u; v=-v; w=-w; }
  double FixW()
    {
    double res=1.-2.*(w<0);
    u*=res; v*=res; w*=res;
    return res;
    }
  };

class Baselines
  {
  protected:
    vector<UVW> coord;
    vector<double> f_over_c;
    size_t nrows, nchan;
    double umax, vmax;

  public:
    Baselines() = default;
    template<typename T> Baselines(const cmav<T,2> &coord_,
      const cmav<T,1> &freq, bool negate_v=false)
      {
      constexpr double speedOfLight = 299792458.;
      MR_assert(coord_.shape(1)==3, "dimension mismatch");
      nrows = coord_.shape(0);
      nchan = freq.shape(0);
      f_over_c.resize(nchan);
      double fcmax = 0;
      for (size_t i=0; i<nchan; ++i)
        {
        MR_assert(freq(i)>0, "negative channel frequency encountered");
        f_over_c[i] = freq(i)/speedOfLight;
        fcmax = max(fcmax, abs(f_over_c[i]));
        }
      coord.resize(nrows);
      double vfac = negate_v ? -1 : 1;
      umax=vmax=0;
      for (size_t i=0; i<coord.size(); ++i)
        {
        coord[i] = UVW(coord_(i,0), vfac*coord_(i,1), coord_(i,2));
        umax = max(umax, abs(coord_(i,0)));
        vmax = max(vmax, abs(coord_(i,1)));
        }
      umax *= fcmax;
      vmax *= fcmax;
      }

    UVW effectiveCoord(size_t row, size_t chan) const
      { return coord[row]*f_over_c[chan]; }
    double absEffectiveW(size_t row, size_t chan) const
      { return abs(coord[row].w*f_over_c[chan]); }
    UVW baseCoord(size_t row) const
      { return coord[row]; }
    void prefetchRow(size_t row) const
      { DUCC0_PREFETCH_R(&coord[row]); }
    double ffact(size_t chan) const
      { return f_over_c[chan];}
    size_t Nrows() const { return nrows; }
    size_t Nchannels() const { return nchan; }
    double Umax() const { return umax; }
    double Vmax() const { return vmax; }

    const vector<UVW> &getUVW_raw() const { return coord; }
    const vector<double> &get_f_over_c() const { return f_over_c; }
  };


constexpr int logsquare=4;

template<typename Tcalc, typename Tacc, typename Tms, typename Timg> class Params
  {
  private:
    bool gridding;
    TimerHierarchy timers;
    const cmav<complex<Tms>,2> &ms_in;
    vmav<complex<Tms>,2> &ms_out;
    const cmav<Timg,2> &dirty_in;
    vmav<Timg,2> &dirty_out;
    const cmav<Tms,2> &wgt;
    const cmav<uint8_t,2> &mask;
    double pixsize_x, pixsize_y;
    size_t nxdirty, nydirty;
    double epsilon;
    bool do_wgridding;
    size_t nthreads;
    size_t verbosity;
    bool negate_v, divide_by_n;
    double sigma_min, sigma_max;

    Baselines bl;
    VVR ranges;
    double wmin_d, wmax_d;
    size_t nvis;
    double wmin, dw;
    size_t nplanes;
    double nm1min, nm1max;

    double lshift, mshift, nshift;
    bool shifting, lmshift, no_nshift;

    size_t nu, nv;
    double ofactor;

    shared_ptr<HornerKernel> krn;

    size_t supp, nsafe;
    double ushift, vshift;
    int maxiu0, maxiv0;
    size_t vlim;
    bool uv_side_fast;
    vector<rangeset<int>> uranges, vranges;

    bool gpu;

    static_assert(sizeof(Tcalc)<=sizeof(Tacc), "bad type combination");
    static_assert(sizeof(Tms)<=sizeof(Tcalc), "bad type combination");
    static_assert(sizeof(Timg)<=sizeof(Tcalc), "bad type combination");

    static double phase(double x, double y, double w, bool adjoint, double nshift)
      {
      double tmp = 1.-x-y;
      if (tmp<=0) return 0; // no phase factor beyond the horizon
      double nm1 = (-x-y)/(sqrt(tmp)+1); // more accurate form of sqrt(1-x-y)-1
      double phs = w*(nm1+nshift);
      if (adjoint) phs *= -1;
      if constexpr (is_same<Tcalc, double>::value)
        return twopi*phs;
      // we are reducing accuracy, so let's better do range reduction first
      return twopi*(phs-floor(phs));
      }

    void grid2dirty_post(vmav<Tcalc,2> &tmav, vmav<Timg,2> &dirty) const
      {
      checkShape(dirty.shape(), {nxdirty, nydirty});
      auto cfu = krn->corfunc(nxdirty/2+1, 1./nu, nthreads);
      auto cfv = krn->corfunc(nydirty/2+1, 1./nv, nthreads);
      execParallel(nxdirty, nthreads, [&](size_t lo, size_t hi)
        {
        for (auto i=lo; i<hi; ++i)
          {
          int icfu = abs(int(nxdirty/2)-int(i));
          for (size_t j=0; j<nydirty; ++j)
            {
            int icfv = abs(int(nydirty/2)-int(j));
            size_t i2 = nu-nxdirty/2+i;
            if (i2>=nu) i2-=nu;
            size_t j2 = nv-nydirty/2+j;
            if (j2>=nv) j2-=nv;
            dirty(i,j) = Timg(tmav(i2,j2)*cfu[icfu]*cfv[icfv]);
            }
          }
        });
      }
    void grid2dirty_post2(vmav<complex<Tcalc>,2> &tmav, vmav<Timg,2> &dirty, double w) const
      {
      checkShape(dirty.shape(), {nxdirty,nydirty});
      double x0 = lshift-0.5*nxdirty*pixsize_x,
             y0 = mshift-0.5*nydirty*pixsize_y;
      size_t nxd = lmshift ? nxdirty : (nxdirty/2+1);
      execParallel(nxd, nthreads, [&](size_t lo, size_t hi)
        {
        vector<complex<Tcalc>> phases(lmshift ? nydirty : (nydirty/2+1));
        vector<Tcalc> buf(lmshift ? nydirty : (nydirty/2+1));
        for (auto i=lo; i<hi; ++i)
          {
          double fx = sqr(x0+i*pixsize_x);
          size_t ix = nu-nxdirty/2+i;
          if (ix>=nu) ix-=nu;
          expi(phases, buf, [&](size_t i)
            { return Tcalc(phase(fx, sqr(y0+i*pixsize_y), w, true, nshift)); });
          if (lmshift)
            for (size_t j=0, jx=nv-nydirty/2; j<nydirty; ++j, jx=(jx+1>=nv)? jx+1-nv : jx+1)
              dirty(i,j) += Timg(tmav(ix,jx).real()*phases[j].real()
                               - tmav(ix,jx).imag()*phases[j].imag());
          else
            {
            size_t i2 = nxdirty-i;
            size_t ix2 = nu-nxdirty/2+i2;
            if (ix2>=nu) ix2-=nu;
            if ((i>0)&&(i<i2))
              for (size_t j=0, jx=nv-nydirty/2; j<nydirty; ++j, jx=(jx+1>=nv)? jx+1-nv : jx+1)
                {
                size_t j2 = min(j, nydirty-j);
                Tcalc re = phases[j2].real(), im = phases[j2].imag();
                dirty(i ,j) += Timg(tmav(ix ,jx).real()*re - tmav(ix ,jx).imag()*im);
                dirty(i2,j) += Timg(tmav(ix2,jx).real()*re - tmav(ix2,jx).imag()*im);
                }
            else
              for (size_t j=0, jx=nv-nydirty/2; j<nydirty; ++j, jx=(jx+1>=nv)? jx+1-nv : jx+1)
                {
                size_t j2 = min(j, nydirty-j);
                Tcalc re = phases[j2].real(), im = phases[j2].imag();
                dirty(i,j) += Timg(tmav(ix,jx).real()*re - tmav(ix,jx).imag()*im); // lower left
                }
            }
          }
        });
      }

    void grid2dirty_overwrite(vmav<Tcalc,2> &grid, vmav<Timg,2> &dirty)
      {
      timers.push("FFT");
      checkShape(grid.shape(), {nu,nv});
      hartley2_2D(grid, vlim, uv_side_fast, nthreads);
      timers.poppush("grid correction");
      grid2dirty_post(grid, dirty);
      timers.pop();
      }

    void grid2dirty_c_overwrite_wscreen_add
      (vmav<complex<Tcalc>,2> &grid, vmav<Timg,2> &dirty, double w, size_t iplane)
      {
      timers.push("FFT");
      checkShape(grid.shape(), {nu,nv});
      vfmav<complex<Tcalc>> inout(grid);

      const auto &rsu(uranges[iplane]);
      const auto &rsv(vranges[iplane]);
      auto cost_ufirst = nxdirty*log(nv)*nv + rsv.nval()*log(nu)*nu;
      auto cost_vfirst = nydirty*log(nu)*nu + rsu.nval()*log(nv)*nv;
      if (cost_ufirst<cost_vfirst)
        {
        for (size_t i=0; i<rsv.nranges(); ++i)
          {
          auto inout_tmp = inout.subarray({{},{size_t(rsv.ivbegin(i)), size_t(rsv.ivend(i))}});
          c2c(inout_tmp, inout_tmp, {0}, BACKWARD, Tcalc(1), nthreads);
          }
        auto inout_lo = inout.subarray({{0,nxdirty/2},{}});
        c2c(inout_lo, inout_lo, {1}, BACKWARD, Tcalc(1), nthreads);
        auto inout_hi = inout.subarray({{inout.shape(0)-nxdirty/2, MAXIDX},{}});
        c2c(inout_hi, inout_hi, {1}, BACKWARD, Tcalc(1), nthreads);
        }
      else
        {
        for (size_t i=0; i<rsu.nranges(); ++i)
          {
          auto inout_tmp = inout.subarray({{size_t(rsu.ivbegin(i)), size_t(rsu.ivend(i))}, {}});
          c2c(inout_tmp, inout_tmp, {1}, BACKWARD, Tcalc(1), nthreads);
          }
        auto inout_lo = inout.subarray({{}, {0,nydirty/2}});
        c2c(inout_lo, inout_lo, {0}, BACKWARD, Tcalc(1), nthreads);
        auto inout_hi = inout.subarray({{},{inout.shape(1)-nydirty/2, MAXIDX}});
        c2c(inout_hi, inout_hi, {0}, BACKWARD, Tcalc(1), nthreads);
        }

      timers.poppush("wscreen+grid correction");
      grid2dirty_post2(grid, dirty, w);
      timers.pop();
      }

    void dirty2grid_pre(const cmav<Timg,2> &dirty, vmav<Tcalc,2> &grid)
      {
      timers.push("zeroing grid");
      checkShape(dirty.shape(), {nxdirty, nydirty});
      checkShape(grid.shape(), {nu, nv});
      auto cfu = krn->corfunc(nxdirty/2+1, 1./nu, nthreads);
      auto cfv = krn->corfunc(nydirty/2+1, 1./nv, nthreads);
      // only zero the parts of the grid that are not filled afterwards anyway
      { auto a0 = subarray<2>(grid, {{0,nxdirty/2}, {nydirty/2,nv-nydirty/2+1}}); quickzero(a0, nthreads); }
      { auto a0 = subarray<2>(grid, {{nxdirty/2, nu-nxdirty/2+1}, {}}); quickzero(a0, nthreads); }
      { auto a0 = subarray<2>(grid, {{nu-nxdirty/2+1,MAXIDX}, {nydirty/2, nv-nydirty/2+1}}); quickzero(a0, nthreads); }
      timers.poppush("grid correction");
      execParallel(nxdirty, nthreads, [&](size_t lo, size_t hi)
        {
        for (auto i=lo; i<hi; ++i)
          {
          int icfu = abs(int(nxdirty/2)-int(i));
          for (size_t j=0; j<nydirty; ++j)
            {
            int icfv = abs(int(nydirty/2)-int(j));
            size_t i2 = nu-nxdirty/2+i;
            if (i2>=nu) i2-=nu;
            size_t j2 = nv-nydirty/2+j;
            if (j2>=nv) j2-=nv;
            grid(i2,j2) = dirty(i,j)*Tcalc(cfu[icfu]*cfv[icfv]);
            }
          }
        });
      timers.pop();
      }
    void dirty2grid_pre2(const cmav<Timg,2> &dirty, vmav<complex<Tcalc>,2> &grid, double w)
      {
      timers.push("zeroing grid");
      checkShape(dirty.shape(), {nxdirty, nydirty});
      checkShape(grid.shape(), {nu, nv});
      // only zero the parts of the grid that are not filled afterwards anyway
      { auto a0 = subarray<2>(grid, {{0,nxdirty/2}, {nydirty/2, nv-nydirty/2+1}}); quickzero(a0, nthreads); }
      { auto a0 = subarray<2>(grid, {{nxdirty/2,nu-nxdirty/2+1}, {}}); quickzero(a0, nthreads); }
      { auto a0 = subarray<2>(grid, {{nu-nxdirty/2+1,MAXIDX}, {nydirty/2,nv-nydirty/2+1}}); quickzero(a0, nthreads); }
      timers.poppush("wscreen+grid correction");
      double x0 = lshift-0.5*nxdirty*pixsize_x,
             y0 = mshift-0.5*nydirty*pixsize_y;
      size_t nxd = lmshift ? nxdirty : (nxdirty/2+1);
      execParallel(nxd, nthreads, [&](size_t lo, size_t hi)
        {
        vector<complex<Tcalc>> phases(lmshift ? nydirty : (nydirty/2+1)); 
        vector<Tcalc> buf(lmshift ? nydirty : (nydirty/2+1)); 
        for(auto i=lo; i<hi; ++i)
          {
          double fx = sqr(x0+i*pixsize_x);
          size_t ix = nu-nxdirty/2+i;
          if (ix>=nu) ix-=nu;
          expi(phases, buf, [&](size_t i)
            { return Tcalc(phase(fx, sqr(y0+i*pixsize_y), w, false, nshift)); });
          if (lmshift)
            for (size_t j=0, jx=nv-nydirty/2; j<nydirty; ++j, jx=(jx+1>=nv)? jx+1-nv : jx+1)
              grid(ix,jx) = Tcalc(dirty(i,j))*phases[j];
          else
            {
            size_t i2 = nxdirty-i;
            size_t ix2 = nu-nxdirty/2+i2;
            if (ix2>=nu) ix2-=nu;
            if ((i>0)&&(i<i2))
              for (size_t j=0, jx=nv-nydirty/2; j<nydirty; ++j, jx=(jx+1>=nv)? jx+1-nv : jx+1)
                {
                size_t j2 = min(j, nydirty-j);
                grid(ix ,jx) = Tcalc(dirty(i ,j))*phases[j2]; // lower left
                grid(ix2,jx) = Tcalc(dirty(i2,j))*phases[j2]; // lower right
                }
            else
              for (size_t j=0, jx=nv-nydirty/2; j<nydirty; ++j, jx=(jx+1>=nv)? jx+1-nv : jx+1)
                grid(ix,jx) = Tcalc(dirty(i,j))*phases[min(j, nydirty-j)]; // lower left
            }
          }
        });
      timers.pop();
      }

    void dirty2grid(const cmav<Timg,2> &dirty, vmav<Tcalc,2> &grid)
      {
      dirty2grid_pre(dirty, grid);
      timers.push("FFT");
      hartley2_2D(grid, vlim, !uv_side_fast, nthreads);
      timers.pop();
      }

    void dirty2grid_c_wscreen(const cmav<Timg,2> &dirty,
      vmav<complex<Tcalc>,2> &grid, double w, size_t iplane)
      {
      dirty2grid_pre2(dirty, grid, w);
      timers.push("FFT");
      vfmav<complex<Tcalc>> inout(grid);

      const auto &rsu(uranges[iplane]);
      const auto &rsv(vranges[iplane]);
      auto cost_ufirst = nydirty*log(nu)*nu + rsu.nval()*log(nv)*nv;
      auto cost_vfirst = nxdirty*log(nv)*nv + rsv.nval()*log(nu)*nu;
      if (cost_ufirst<cost_vfirst)
        {
        auto inout_lo = inout.subarray({{}, {0,nydirty/2}});
        c2c(inout_lo, inout_lo, {0}, FORWARD, Tcalc(1), nthreads);
        auto inout_hi = inout.subarray({{},{inout.shape(1)-nydirty/2, MAXIDX}});
        c2c(inout_hi, inout_hi, {0}, FORWARD, Tcalc(1), nthreads);
        for (size_t i=0; i<rsu.nranges(); ++i)
          {
          auto inout_tmp = inout.subarray({{size_t(rsu.ivbegin(i)), size_t(rsu.ivend(i))}, {}});
          c2c(inout_tmp, inout_tmp, {1}, FORWARD, Tcalc(1), nthreads);
          }
        }
      else
        {
        auto inout_lo = inout.subarray({{0,nxdirty/2},{}});
        c2c(inout_lo, inout_lo, {1}, FORWARD, Tcalc(1), nthreads);
        auto inout_hi = inout.subarray({{inout.shape(0)-nxdirty/2, MAXIDX},{}});
        c2c(inout_hi, inout_hi, {1}, FORWARD, Tcalc(1), nthreads);
        for (size_t i=0; i<rsv.nranges(); ++i)
          {
          auto inout_tmp = inout.subarray({{},{size_t(rsv.ivbegin(i)), size_t(rsv.ivend(i))}});
          c2c(inout_tmp, inout_tmp, {0}, FORWARD, Tcalc(1), nthreads);
          }
        }
      timers.pop();
      }

    [[gnu::always_inline]] void getpix(double u_in, double v_in, double &u, double &v, int &iu0, int &iv0) const
      {
      u = u_in*pixsize_x;
      u = (u-floor(u))*nu;
      iu0 = min(int(u+ushift)-int(nu), maxiu0);
      u -= iu0;
      v = v_in*pixsize_y;
      v = (v-floor(v))*nv;
      iv0 = min(int(v+vshift)-int(nv), maxiv0);
      v -= iv0;
      }

    void countRanges()
      {
      timers.push("building index");
      size_t nrow=bl.Nrows(),
             nchan=bl.Nchannels();

      if (do_wgridding)
        {
        dw = 0.5/ofactor/max(abs(nm1max+nshift), abs(nm1min+nshift));
        nplanes = size_t((wmax_d-wmin_d)/dw+supp);
        MR_assert(nplanes<(size_t(1)<<16), "too many w planes");
        wmin = (wmin_d+wmax_d)*0.5 - 0.5*(nplanes-1)*dw;
        }
      else
        dw = wmin = nplanes = 0;
      size_t nbunch = do_wgridding ? supp : 1;
      // we want a maximum deviation of 1% in gridding time between threads
      constexpr double max_asymm = 0.01;
      size_t max_allowed = size_t(nvis/double(nbunch*nthreads)*max_asymm);

      struct tmp2
        {
        size_t sz=0;
        vector<vector<RowchanRange>> v;
        void add(const RowchanRange &rng, size_t max_allowed)
          {
          if (v.empty() || (sz>=max_allowed))
            { v.emplace_back(); sz=0; }
          v.back().push_back(rng);
          sz += rng.ch_end-rng.ch_begin;
          }
        };
      using Vmap = map<Uvwidx, tmp2>;
      struct bufmap
        {
        Vmap m;
        mutex mut;
        uint64_t dummy[8]; // separator to keep every entry on a different cache line
        };
      checkShape(wgt.shape(),{nrow,nchan});
      checkShape(ms_in.shape(), {nrow,nchan});
      checkShape(mask.shape(), {nrow,nchan});

      size_t ntiles_u = (nu>>logsquare) + 20,
             ntiles_v = (nv>>logsquare) + 20;
      vector<bufmap> buf(ntiles_u*ntiles_v);
      auto chunk = max<size_t>(1, nrow/(20*nthreads));
      auto xdw = 1./dw;
      auto shift = dw-(0.5*supp*dw)-wmin;
      execDynamic(nrow, nthreads, chunk, [&](Scheduler &sched)
        {
        vector<pair<uint16_t, uint16_t>> interbuf;
        while (auto rng=sched.getNext())
        for(auto irow=rng.lo; irow<rng.hi; ++irow)
          {
          bool on=false;
          Uvwidx uvwlast(0,0,0);
          size_t chan0=0;

          auto flush=[&]()
            {
            if (interbuf.empty()) return;
            auto tileidx = uvwlast.tile_u + ntiles_u*uvwlast.tile_v;
            lock_guard<mutex> lock(buf[tileidx].mut);
            auto &loc(buf[tileidx].m[uvwlast]);
            for (auto &x: interbuf)
              loc.add(RowchanRange(irow, x.first, x.second), max_allowed);
            interbuf.clear();
            };
          auto add=[&](uint16_t cb, uint16_t ce)
            { interbuf.emplace_back(cb, ce); };

          for (size_t ichan=0; ichan<nchan; ++ichan)
            {
            if (norm(ms_in(irow,ichan))*wgt(irow,ichan)*mask(irow,ichan)!=0)
              {
              auto uvw = bl.effectiveCoord(irow, ichan);
              uvw.FixW();
              double udum, vdum;
              int iu0, iv0, iw;
              getpix(uvw.u, uvw.v, udum, vdum, iu0, iv0);
              iu0 = (iu0+nsafe)>>logsquare;
              iv0 = (iv0+nsafe)>>logsquare;
              iw = do_wgridding ? max(0,int((uvw.w+shift)*xdw)) : 0;
              Uvwidx uvwcur(iu0, iv0, iw);
              if (!on) // new active region
                {
                on=true;
                if (uvwlast!=uvwcur) flush();
                uvwlast=uvwcur; chan0=ichan;
                }
              else if (uvwlast!=uvwcur) // change of active region
                {
                add(chan0, ichan);
                flush();
                uvwlast=uvwcur; chan0=ichan;
                }
              }
            else if (on) // end of active region
              {
              add(chan0, ichan);
              on=false;
              }
            }
          if (on) // end of active region at last channel
            add(chan0, nchan);
          flush();
          }
        });

      size_t total=0;
      for (const auto &x: buf)
        for (const auto &y: x.m)
          total += y.second.v.size();
      ranges.reserve(total);
      for (auto &x: buf)
        for (auto &y: x.m)
          for (auto &z: y.second.v)
            ranges.emplace_back(y.first, move(z));

      // compute which grid regions are required
      if (do_wgridding)
        {
        timers.poppush("grid regions");
        vmav<unsigned char, 2> tmpu({nplanes,(nu>>logsquare)+1}),
                               tmpv({nplanes,(nv>>logsquare)+1});
        for (const auto &rng: ranges)
          for (size_t i=0; i<supp; ++i)
            {
            tmpu(rng.first.minplane+i, rng.first.tile_u) = 1;
            tmpv(rng.first.minplane+i, rng.first.tile_v) = 1;
            }
        uranges.resize(nplanes);
        vranges.resize(nplanes);
        constexpr int tilesize = 1<<logsquare;
        for (size_t i=0; i<nplanes; ++i)
          {
          auto &rsu(uranges[i]);
          auto &rsv(vranges[i]);
          for (size_t j=0; j<tmpu.shape(1); ++j)
            if (tmpu(i,j))
              rsu.add(j*tilesize-int(supp/2)-1, (j+1)*tilesize+int(supp/2)+1);
          // handle wraparound
          if (!rsu.empty() && rsu.ivbegin(0)<0)
            {
            int tmp = rsu.ivbegin(0);
            rsu.remove(tmp,0);
            rsu.add(nu+tmp, nu);
            }
          if (!rsu.empty() && rsu.ivend(rsu.size()-1)>int(nu))
            {
            int tmp = rsu.ivend(rsu.size()-1);
            rsu.remove(nu,tmp);
            rsu.add(0, tmp-nu);
            }
          for (size_t j=0; j<tmpv.shape(1); ++j)
            if (tmpv(i,j))
              rsv.add(j*tilesize-int(supp/2)-1, (j+1)*tilesize+int(supp/2)+1);
          // handle wraparound
          if (!rsv.empty() && rsv.ivbegin(0)<0)
            {
            int tmp = rsv.ivbegin(0);
            rsv.remove(tmp,0);
            rsv.add(nv+tmp, nv);
            }
          if (!rsv.empty() && rsv.ivend(rsv.size()-1)>int(nv))
            {
            int tmp = rsv.ivend(rsv.size()-1);
            rsv.remove(nv,tmp);
            rsv.add(0, tmp-nv);
            }
          }
        }
      timers.pop();
      }

    template<size_t supp, bool wgrid> class HelperX2g2
      {
      public:
        static constexpr size_t vlen = mysimd<Tacc>::size();
        static constexpr size_t nvec = (supp+vlen-1)/vlen;

      private:
        static constexpr int nsafe = (supp+1)/2;
        static constexpr int su = 2*nsafe+(1<<logsquare);
        static constexpr int sv = 2*nsafe+(1<<logsquare);
        static constexpr int svvec = sv+vlen-1;
        static constexpr double xsupp=2./supp;
        const Params *parent;
        TemplateKernel<supp, mysimd<Tacc>> tkrn;
        vmav<complex<Tcalc>,2> &grid;
        int iu0, iv0; // start index of the current visibility
        int bu0, bv0; // start index of the current buffer

        vmav<Tacc,2> bufr, bufi;
        Tacc *px0r, *px0i;
        double w0, xdw;
        vector<mutex> &locks;

        DUCC0_NOINLINE void dump()
          {
          int inu = int(parent->nu);
          int inv = int(parent->nv);
          if (bu0<-nsafe) return; // nothing written into buffer yet

          int idxu = (bu0+inu)%inu;
          int idxv0 = (bv0+inv)%inv;
          for (int iu=0; iu<su; ++iu)
            {
            int idxv = idxv0;
            {
            lock_guard<mutex> lock(locks[idxu]);
            for (int iv=0; iv<sv; ++iv)
              {
              grid(idxu,idxv) += complex<Tcalc>(Tcalc(bufr(iu,iv)), Tcalc(bufi(iu,iv)));
              bufr(iu,iv) = bufi(iu,iv) = 0;
              if (++idxv>=inv) idxv=0;
              }
            }
            if (++idxu>=inu) idxu=0;
            }
          }

      public:
        Tacc * DUCC0_RESTRICT p0r, * DUCC0_RESTRICT p0i;
        union kbuf {
          Tacc scalar[2*nvec*vlen];
          mysimd<Tacc> simd[2*nvec];
#if defined(_MSC_VER)
          kbuf() {}
#endif
          };
        kbuf buf;

        HelperX2g2(const Params *parent_, vmav<complex<Tcalc>,2> &grid_,
          vector<mutex> &locks_, double w0_=-1, double dw_=-1)
          : parent(parent_), tkrn(*parent->krn), grid(grid_),
            iu0(-1000000), iv0(-1000000),
            bu0(-1000000), bv0(-1000000),
            bufr({size_t(su),size_t(svvec)}),
            bufi({size_t(su),size_t(svvec)}),
            px0r(bufr.data()), px0i(bufi.data()),
            w0(w0_),
            xdw(1./dw_),
            locks(locks_)
          { checkShape(grid.shape(), {parent->nu,parent->nv}); }
        ~HelperX2g2() { dump(); }

        constexpr int lineJump() const { return svvec; }

        [[gnu::always_inline]] [[gnu::hot]] void prep(const UVW &in, size_t nth=0)
          {
          double ufrac, vfrac;
          auto iu0old = iu0;
          auto iv0old = iv0;
          parent->getpix(in.u, in.v, ufrac, vfrac, iu0, iv0);
          auto x0 = -ufrac*2+(supp-1);
          auto y0 = -vfrac*2+(supp-1);
          if constexpr(wgrid)
            tkrn.eval2s(Tacc(x0), Tacc(y0), Tacc(xdw*(w0-in.w)), nth, &buf.simd[0]);
          else
            tkrn.eval2(Tacc(x0), Tacc(y0), &buf.simd[0]);
          if ((iu0==iu0old) && (iv0==iv0old)) return;
          if ((iu0<bu0) || (iv0<bv0) || (iu0+int(supp)>bu0+su) || (iv0+int(supp)>bv0+sv))
            {
            dump();
            bu0=((((iu0+nsafe)>>logsquare)<<logsquare))-nsafe;
            bv0=((((iv0+nsafe)>>logsquare)<<logsquare))-nsafe;
            }
          auto ofs = (iu0-bu0)*svvec + iv0-bv0;
          p0r = px0r+ofs;
          p0i = px0i+ofs;
          }
      };


    template<size_t supp, bool wgrid> class HelperG2x2
      {
      public:
        static constexpr size_t vlen = mysimd<Tcalc>::size();
        static constexpr size_t nvec = (supp+vlen-1)/vlen;

      private:
        static constexpr int nsafe = (supp+1)/2;
        static constexpr int su = 2*nsafe+(1<<logsquare);
        static constexpr int sv = 2*nsafe+(1<<logsquare);
        static constexpr int svvec = sv+vlen-1;
        static constexpr double xsupp=2./supp;
        const Params *parent;

        TemplateKernel<supp, mysimd<Tcalc>> tkrn;
        const cmav<complex<Tcalc>,2> &grid;
        int iu0, iv0; // start index of the current visibility
        int bu0, bv0; // start index of the current buffer

        vmav<Tcalc,2> bufr, bufi;
        const Tcalc *px0r, *px0i;
        double w0, xdw;

        DUCC0_NOINLINE void load()
          {
          int inu = int(parent->nu);
          int inv = int(parent->nv);
          int idxu = (bu0+inu)%inu;
          int idxv0 = (bv0+inv)%inv;
          for (int iu=0; iu<su; ++iu)
            {
            int idxv = idxv0;
            for (int iv=0; iv<sv; ++iv)
              {
              bufr(iu,iv) = grid(idxu, idxv).real();
              bufi(iu,iv) = grid(idxu, idxv).imag();
              if (++idxv>=inv) idxv=0;
              }
            if (++idxu>=inu) idxu=0;
            }
          }

      public:
        const Tcalc * DUCC0_RESTRICT p0r, * DUCC0_RESTRICT p0i;
        union kbuf {
          Tcalc scalar[2*nvec*vlen];
          mysimd<Tcalc> simd[2*nvec];
#if defined(_MSC_VER)
          kbuf() {}
#endif
          };
        kbuf buf;

        HelperG2x2(const Params *parent_, const cmav<complex<Tcalc>,2> &grid_,
          double w0_=-1, double dw_=-1)
          : parent(parent_), tkrn(*parent->krn), grid(grid_),
            iu0(-1000000), iv0(-1000000),
            bu0(-1000000), bv0(-1000000),
            bufr({size_t(su),size_t(svvec)}),
            bufi({size_t(su),size_t(svvec)}),
            px0r(bufr.data()), px0i(bufi.data()),
            w0(w0_),
            xdw(1./dw_)
          { checkShape(grid.shape(), {parent->nu,parent->nv}); }

        constexpr int lineJump() const { return svvec; }

        [[gnu::always_inline]] [[gnu::hot]] void prep(const UVW &in, size_t nth=0)
          {
          double ufrac, vfrac;
          auto iu0old = iu0;
          auto iv0old = iv0;
          parent->getpix(in.u, in.v, ufrac, vfrac, iu0, iv0);
          auto x0 = -ufrac*2+(supp-1);
          auto y0 = -vfrac*2+(supp-1);
          if constexpr(wgrid)
            tkrn.eval2s(Tcalc(x0), Tcalc(y0), Tcalc(xdw*(w0-in.w)), nth, &buf.simd[0]);
          else
            tkrn.eval2(Tcalc(x0), Tcalc(y0), &buf.simd[0]);
          if ((iu0==iu0old) && (iv0==iv0old)) return;
          if ((iu0<bu0) || (iv0<bv0) || (iu0+int(supp)>bu0+su) || (iv0+int(supp)>bv0+sv))
            {
            bu0=((((iu0+nsafe)>>logsquare)<<logsquare))-nsafe;
            bv0=((((iv0+nsafe)>>logsquare)<<logsquare))-nsafe;
            load();
            }
          auto ofs = (iu0-bu0)*svvec + iv0-bv0;
          p0r = px0r+ofs;
          p0i = px0i+ofs;
          }
      };

    void compute_phases(vector<complex<Tcalc>> &phases, vector<Tcalc> &buf,
      Tcalc imflip, const UVW &bcoord, const RowchanRange &rcr)
      {
      phases.resize(rcr.ch_end-rcr.ch_begin);
      buf.resize(rcr.ch_end-rcr.ch_begin);
      double fct = imflip*(bcoord.u*lshift + bcoord.v*mshift + bcoord.w*nshift);
      expi(phases, buf, [&](size_t i) {
                      auto tmp = fct*bl.ffact(rcr.ch_begin+i);
                      if constexpr (is_same<double, Tcalc>::value)
                        return Tcalc(twopi*tmp);
                      // we are reducing accuracy,
                      // so let's better do range reduction first
                      return Tcalc(twopi*(tmp-floor(tmp)));
                      });
      }

    template<size_t SUPP, bool wgrid> [[gnu::hot]] void x2grid_c_helper
      (size_t supp, vmav<complex<Tcalc>,2> &grid, size_t p0, double w0)
      {
      if constexpr (SUPP>=8)
        if (supp<=SUPP/2) return x2grid_c_helper<SUPP/2, wgrid>(supp, grid, p0, w0);
      if constexpr (SUPP>4)
        if (supp<SUPP) return x2grid_c_helper<SUPP-1, wgrid>(supp, grid, p0, w0);
      MR_assert(supp==SUPP, "requested support ou of range");

      vector<mutex> locks(nu);

      execDynamic(ranges.size(), nthreads, wgrid ? SUPP : 1, [&](Scheduler &sched)
        {
        constexpr auto vlen=mysimd<Tacc>::size();
        constexpr auto NVEC((SUPP+vlen-1)/vlen);
        HelperX2g2<SUPP,wgrid> hlp(this, grid, locks, w0, dw);
        constexpr auto jump = hlp.lineJump();
        const auto * DUCC0_RESTRICT ku = hlp.buf.scalar;
        const auto * DUCC0_RESTRICT kv = hlp.buf.simd+NVEC;
        vector<complex<Tcalc>> phases;
        vector<Tcalc> buf;

        while (auto rng=sched.getNext()) for(auto ix_=rng.lo; ix_<rng.hi; ++ix_)
          {
auto ix = ix_+ranges.size()/2; if (ix>=ranges.size()) ix -=ranges.size();
          const auto &uvwidx(ranges[ix].first);
          if ((!wgrid) || ((uvwidx.minplane+SUPP>p0)&&(uvwidx.minplane<=p0)))
            {
//bool lastplane = (!wgrid) || (uvwidx.minplane+SUPP-1==p0);
            size_t nth = p0-uvwidx.minplane;
            for (size_t cnt=0; cnt<ranges[ix].second.size(); ++cnt)
              {
              const auto &rcr(ranges[ix].second[cnt]);
              if (cnt+1<ranges[ix].second.size())
                {
                const auto &nextrcr(ranges[ix].second[cnt+1]);
                DUCC0_PREFETCH_R(&wgt(nextrcr.row, nextrcr.ch_begin));
                DUCC0_PREFETCH_R(&ms_in(nextrcr.row, nextrcr.ch_begin));
                bl.prefetchRow(nextrcr.row);
                }
              size_t row = rcr.row;
              auto bcoord = bl.baseCoord(row);
              auto imflip = Tcalc(bcoord.FixW());
              if (shifting)
                compute_phases(phases, buf, imflip, bcoord, rcr);
              for (size_t ch=rcr.ch_begin; ch<rcr.ch_end; ++ch)
                {
                auto coord = bcoord*bl.ffact(ch);
                hlp.prep(coord, nth);
                auto v(ms_in(row, ch));
                if (shifting)
                  v*=phases[ch-rcr.ch_begin];
                v*=wgt(row, ch);

                if constexpr (NVEC==1)
                  {
                  mysimd<Tacc> vr=v.real()*kv[0], vi=v.imag()*imflip*kv[0];
                  for (size_t cu=0; cu<SUPP; ++cu)
                    {
                    auto * DUCC0_RESTRICT pxr = hlp.p0r+cu*jump;
                    auto * DUCC0_RESTRICT pxi = hlp.p0i+cu*jump;
                    auto tr = mysimd<Tacc>(pxr,element_aligned_tag());
                    auto ti = mysimd<Tacc>(pxi,element_aligned_tag());
                    tr += vr*ku[cu];
                    ti += vi*ku[cu];
                    tr.copy_to(pxr,element_aligned_tag());
                    ti.copy_to(pxi,element_aligned_tag());
                    }
                  }
                else
                  {
                  mysimd<Tacc> vr(v.real()), vi(v.imag()*imflip);
                  for (size_t cu=0; cu<SUPP; ++cu)
                    {
                    mysimd<Tacc> tmpr=vr*ku[cu], tmpi=vi*ku[cu];
                    for (size_t cv=0; cv<NVEC; ++cv)
                      {
                      auto * DUCC0_RESTRICT pxr = hlp.p0r+cu*jump+cv*hlp.vlen;
                      auto * DUCC0_RESTRICT pxi = hlp.p0i+cu*jump+cv*hlp.vlen;
                      auto tr = mysimd<Tacc>(pxr,element_aligned_tag());
                      tr += tmpr*kv[cv];
                      tr.copy_to(pxr,element_aligned_tag());
                      auto ti = mysimd<Tacc>(pxi, element_aligned_tag());
                      ti += tmpi*kv[cv];
                      ti.copy_to(pxi,element_aligned_tag());
                      }
                    }
                  }
                }
              }
            }
          }
        });
      }

    template<bool wgrid> void x2grid_c(vmav<complex<Tcalc>,2> &grid,
      size_t p0, double w0=-1)
      {
      checkShape(grid.shape(), {nu, nv});
      constexpr size_t maxsupp = is_same<Tacc, double>::value ? 16 : 8;
      x2grid_c_helper<maxsupp, wgrid>(supp, grid, p0, w0);
      }

    template<size_t SUPP, bool wgrid> [[gnu::hot]] void grid2x_c_helper
      (size_t supp, const cmav<complex<Tcalc>,2> &grid, size_t p0, double w0)
      {
      if constexpr (SUPP>=8)
        if (supp<=SUPP/2) return grid2x_c_helper<SUPP/2, wgrid>(supp, grid, p0, w0);
      if constexpr (SUPP>4)
        if (supp<SUPP) return grid2x_c_helper<SUPP-1, wgrid>(supp, grid, p0, w0);
      MR_assert(supp==SUPP, "requested support ou of range");

      // Loop over sampling points
      execDynamic(ranges.size(), nthreads, wgrid ? SUPP : 1, [&](Scheduler &sched)
        {
        constexpr size_t vlen=mysimd<Tcalc>::size();
        constexpr size_t NVEC((SUPP+vlen-1)/vlen);
        HelperG2x2<SUPP,wgrid> hlp(this, grid, w0, dw);
        constexpr int jump = hlp.lineJump();
        const auto * DUCC0_RESTRICT ku = hlp.buf.scalar;
        const auto * DUCC0_RESTRICT kv = hlp.buf.simd+NVEC;
        vector<complex<Tcalc>> phases;
        vector<Tcalc> buf;

        while (auto rng=sched.getNext()) for(auto ix_=rng.lo; ix_<rng.hi; ++ix_)
          {
auto ix = ix_+ranges.size()/2; if (ix>=ranges.size()) ix -=ranges.size();
          const auto &uvwidx(ranges[ix].first);
          if ((!wgrid) || ((uvwidx.minplane+SUPP>p0)&&(uvwidx.minplane<=p0)))
            {
            bool lastplane = (!wgrid) || (uvwidx.minplane+SUPP-1==p0);
            size_t nth = p0-uvwidx.minplane;
            for (size_t cnt=0; cnt<ranges[ix].second.size(); ++cnt)
              {
              const auto &rcr(ranges[ix].second[cnt]);
              if (cnt+1<ranges[ix].second.size())
                {
                const auto &nextrcr(ranges[ix].second[cnt+1]);
                DUCC0_PREFETCH_R(&wgt(nextrcr.row, nextrcr.ch_begin));
                DUCC0_PREFETCH_R(&ms_out(nextrcr.row, nextrcr.ch_begin));
                DUCC0_PREFETCH_W(&ms_out(nextrcr.row, nextrcr.ch_begin));
                bl.prefetchRow(nextrcr.row);
                }
              size_t row = rcr.row;
              auto bcoord = bl.baseCoord(row);
              auto imflip = Tcalc(bcoord.FixW());
              if (shifting&&lastplane)
                compute_phases(phases, buf, -imflip, bcoord, rcr);
              for (size_t ch=rcr.ch_begin; ch<rcr.ch_end; ++ch)
                {
                auto coord = bcoord*bl.ffact(ch);
                hlp.prep(coord, nth);
                mysimd<Tcalc> rr=0, ri=0;
                if constexpr (NVEC==1)
                  {
                  for (size_t cu=0; cu<SUPP; ++cu)
                    {
                    const auto * DUCC0_RESTRICT pxr = hlp.p0r + cu*jump;
                    const auto * DUCC0_RESTRICT pxi = hlp.p0i + cu*jump;
                    rr += mysimd<Tcalc>(pxr,element_aligned_tag())*ku[cu];
                    ri += mysimd<Tcalc>(pxi,element_aligned_tag())*ku[cu];
                    }
                  rr *= kv[0];
                  ri *= kv[0];
                  }
                else
                  {
                  for (size_t cu=0; cu<SUPP; ++cu)
                    {
                    mysimd<Tcalc> tmpr(0), tmpi(0);
                    for (size_t cv=0; cv<NVEC; ++cv)
                      {
                      const auto * DUCC0_RESTRICT pxr = hlp.p0r + cu*jump + hlp.vlen*cv;
                      const auto * DUCC0_RESTRICT pxi = hlp.p0i + cu*jump + hlp.vlen*cv;
                      tmpr += kv[cv]*mysimd<Tcalc>(pxr,element_aligned_tag());
                      tmpi += kv[cv]*mysimd<Tcalc>(pxi,element_aligned_tag());
                      }
                    rr += ku[cu]*tmpr;
                    ri += ku[cu]*tmpi;
                    }
                  }
                ri *= imflip;
                auto r = hsum_cmplx<Tcalc>(rr,ri);
                ms_out(row, ch) += r;
                if (lastplane)
                  ms_out(row, ch) *= shifting ?
                    complex<Tms>(phases[ch-rcr.ch_begin]*Tcalc(wgt(row, ch))) :
                    wgt(row, ch);
                }
              }
            }
          }
        });
      }

    template<bool wgrid> void grid2x_c(const cmav<complex<Tcalc>,2> &grid,
      size_t p0, double w0=-1)
      {
      checkShape(grid.shape(), {nu, nv});
      constexpr size_t maxsupp = is_same<Tcalc, double>::value ? 16 : 8;
      grid2x_c_helper<maxsupp, wgrid>(supp, grid, p0, w0);
      }

    void apply_global_corrections(vmav<Timg,2> &dirty)
      {
      timers.push("global corrections");
      double x0 = lshift-0.5*nxdirty*pixsize_x,
             y0 = mshift-0.5*nydirty*pixsize_y;
      auto cfu = krn->corfunc(nxdirty/2+1, 1./nu, nthreads);
      auto cfv = krn->corfunc(nydirty/2+1, 1./nv, nthreads);
      size_t nxd = lmshift ? nxdirty : (nxdirty/2+1);
      size_t nyd = lmshift ? nydirty : (nydirty/2+1);
      execParallel(nxd, nthreads, [&](size_t lo, size_t hi)
        {
        for(auto i=lo; i<hi; ++i)
          {
          double fx = sqr(x0+i*pixsize_x);
          for (size_t j=0; j<nyd; ++j)
            {
            double fy = sqr(y0+j*pixsize_y);
            double fct = 0;
            auto tmp = 1-fx-fy;
            if (tmp>=0)
              {
              auto nm1 = (-fx-fy)/(sqrt(tmp)+1); // accurate form of sqrt(1-x-y)-1
              fct = krn->corfunc((nm1+nshift)*dw);
              if (divide_by_n)
                fct /= nm1+1;
              }
            else // beyond the horizon, don't really know what to do here
              fct = divide_by_n ? 0 : krn->corfunc((sqrt(-tmp)-1)*dw);
            if (lmshift)
              {
              auto i2=min(i, nxdirty-i), j2=min(j, nydirty-j);
              fct *= cfu[nxdirty/2-i2]*cfv[nydirty/2-j2];
              dirty(i,j)*=Timg(fct);
              }
            else
              {
              fct *= cfu[nxdirty/2-i]*cfv[nydirty/2-j];
              size_t i2 = nxdirty-i, j2 = nydirty-j;
              dirty(i,j)*=Timg(fct);
              if ((i>0)&&(i<i2))
                {
                dirty(i2,j)*=Timg(fct);
                if ((j>0)&&(j<j2))
                  dirty(i2,j2)*=Timg(fct);
                }
              if ((j>0)&&(j<j2))
                dirty(i,j2)*=Timg(fct);
              }
            }
          }
        });
      timers.pop();
      }

    void report()
      {
      if (verbosity==0) return;
      cout << (gridding ? "Gridding:" : "Degridding:") << endl
           << "  nthreads=" << nthreads << ", "
           << "dirty=(" << nxdirty << "x" << nydirty << "), "
           << "grid=(" << nu << "x" << nv;
      if (do_wgridding) cout << "x" << nplanes;
      cout << "), supp=" << supp
           << ", eps=" << (epsilon * (do_wgridding ? 3 : 2))
           << endl;
      cout << "  nrow=" << bl.Nrows() << ", nchan=" << bl.Nchannels()
           << ", nvis=" << nvis << "/" << (bl.Nrows()*bl.Nchannels()) << endl;
      if (do_wgridding)
        cout << "  w=[" << wmin_d << "; " << wmax_d << "], min(n-1)=" << nm1min
             << ", dw=" << dw << ", wmax/dw=" << wmax_d/dw << endl;
      size_t ovh0 = 0;
      for (const auto &v : ranges)
        ovh0 += v.second.size()*sizeof(RowchanRange);
      ovh0 += ranges.size()*sizeof(VVR);
      size_t ovh1 = nu*nv*sizeof(complex<Tcalc>);             // grid
      if (!do_wgridding)
        ovh1 += nu*nv*sizeof(Tcalc);                          // rgrid
      if (!gridding)
        ovh1 += nxdirty*nydirty*sizeof(Timg);                 // tdirty
      cout << "  memory overhead: "
           << ovh0/double(1<<30) << "GB (index) + "
           << ovh1/double(1<<30) << "GB (2D arrays)" << endl;
      }

    void x2dirty()
      {
      if (do_wgridding)
        {
        timers.push("zeroing dirty image");
        mav_apply([](Timg &v){v=Timg(0);}, nthreads, dirty_out);
        timers.poppush("allocating grid");
        auto grid = vmav<complex<Tcalc>,2>::build_noncritical({nu,nv}, UNINITIALIZED);
        timers.pop();
        for (size_t pl=0; pl<nplanes; ++pl)
          {
          double w = wmin+pl*dw;
          timers.push("zeroing grid");
          quickzero(grid, nthreads);
          timers.poppush("gridding proper");
          x2grid_c<true>(grid, pl, w);
          timers.pop();
          grid2dirty_c_overwrite_wscreen_add(grid, dirty_out, w, pl);
          }
        // correct for w gridding etc.
        apply_global_corrections(dirty_out);
        }
      else
        {
        timers.push("allocating grid");
        auto grid = vmav<complex<Tcalc>,2>::build_noncritical({nu,nv});
        timers.poppush("gridding proper");
        x2grid_c<false>(grid, 0);
        timers.poppush("allocating rgrid");
        auto rgrid = vmav<Tcalc,2>::build_noncritical(grid.shape(), UNINITIALIZED);
        timers.poppush("complex2hartley");
        complex2hartley(grid, rgrid, nthreads);
        timers.pop();
        grid2dirty_overwrite(rgrid, dirty_out);
        }
      }

    void dirty2x()
      {
      if (do_wgridding)
        {
        timers.push("copying dirty image");
        vmav<Timg,2> tdirty({nxdirty,nydirty}, UNINITIALIZED);
        mav_apply([](Timg &a, const Timg &b) {a=b;}, nthreads, tdirty, dirty_in);
        timers.pop();
        // correct for w gridding etc.
        apply_global_corrections(tdirty);
        timers.push("allocating grid");
        auto grid = vmav<complex<Tcalc>,2>::build_noncritical({nu,nv}, UNINITIALIZED);
        timers.pop();
        for (size_t pl=0; pl<nplanes; ++pl)
          {
          double w = wmin+pl*dw;
          dirty2grid_c_wscreen(tdirty, grid, w, pl);
          timers.push("degridding proper");
          grid2x_c<true>(grid, pl, w);
          timers.pop();
          }
        }
      else
        {
        timers.push("allocating grid");
        auto rgrid = vmav<Tcalc,2>::build_noncritical({nu,nv}, UNINITIALIZED);
        timers.pop();
        dirty2grid(dirty_in, rgrid);
        timers.push("allocating grid");
        auto grid = vmav<complex<Tcalc>,2>::build_noncritical(rgrid.shape());
        timers.poppush("hartley2complex");
        hartley2complex(rgrid, grid, nthreads);
        timers.poppush("degridding proper");
        grid2x_c<false>(grid, 0);
        timers.pop();
        }
      }

class Baselines_GPU_prep
  {
  public:
    sycl::buffer<double,2> buf_uvw;
    sycl::buffer<double,1> buf_freq;

    Baselines_GPU_prep(const Baselines &bl)
      : buf_uvw(reinterpret_cast<const double *>(bl.getUVW_raw().data()),
          sycl::range<2>(bl.Nrows(), 3),
          {sycl::property::buffer::use_host_ptr()}),
        buf_freq(make_sycl_buffer(bl.get_f_over_c())) {}
  };

class Baselines_GPU
  {
  protected:
    sycl::accessor<double,2,sycl::access::mode::read> acc_uvw;
    sycl::accessor<double,1,sycl::access::mode::read> acc_f_over_c;
    size_t nrows, nchan;

  public:
    Baselines_GPU(Baselines_GPU_prep &prep, sycl::handler &cgh)
      : acc_uvw(prep.buf_uvw.template get_access<sycl::access::mode::read>(cgh)),
        acc_f_over_c(prep.buf_freq.template get_access<sycl::access::mode::read>(cgh)),
        nrows(acc_uvw.get_range().get(0)),
        nchan(acc_f_over_c.get_range().get(0))
      {
      MR_assert(acc_uvw.get_range().get(1)==3, "dimension mismatch");
      }

    UVW effectiveCoord(size_t row, size_t chan) const
      {
      double f = acc_f_over_c[chan];
      return UVW(acc_uvw[row][0]*f,
                 acc_uvw[row][1]*f,
                 acc_uvw[row][2]*f);
      }
    double absEffectiveW(size_t row, size_t chan) const
      { return abs(acc_uvw[row][2]*acc_f_over_c[chan]); }
    UVW baseCoord(size_t row) const
      {
      return UVW(acc_uvw[row][0],
                 acc_uvw[row][1],
                 acc_uvw[row][2]);
      }
    double ffact(size_t chan) const
      { return acc_f_over_c[chan];}
    size_t Nrows() const { return nrows; }
    size_t Nchannels() const { return nchan; }
  };

class IndexComputer0
  {
  public:
    static constexpr size_t chunksize=1024;
    bool store_tiles;
    vector<uint32_t> row_gpu;
    vector<uint16_t> chbegin_gpu;
    vector<uint16_t> tile_u_gpu, tile_v_gpu;
    vector<uint16_t> minplane_gpu;
    vector<uint32_t> vissum_gpu;
    vector<uint32_t> blocklimits;
    vector<uint32_t> blockstartidx;

    IndexComputer0(const VVR &ranges, bool do_wgridding, bool store_tiles_)
      : store_tiles(store_tiles_)
      {
      size_t nranges=0;
      for (const auto &rng: ranges)
        nranges+=rng.second.size();
      row_gpu.reserve(nranges);
      chbegin_gpu.reserve(nranges);
      vissum_gpu.reserve(nranges+1);
      size_t isamp=0, curtile_u=~uint16_t(0), curtile_v=~uint16_t(0), curminplane=~uint16_t(0);
      size_t accum=0;

      // if necessary, resize some vectors to size 1, because SYCL is unhappy otherwise
      if (!do_wgridding)
        minplane_gpu.resize(1);
      if (!store_tiles)
        {
        tile_u_gpu.resize(1);
        tile_v_gpu.resize(1);
        }
      for (const auto &rng: ranges)
        {
        if ((curtile_u!=rng.first.tile_u)||(curtile_v!=rng.first.tile_v)
          ||(curminplane!=rng.first.minplane))
          {
          blocklimits.push_back(row_gpu.size());
          blockstartidx.push_back(accum);
          isamp=0;
          curtile_u = rng.first.tile_u;
          curtile_v = rng.first.tile_v;
          curminplane = rng.first.minplane;
          if (store_tiles)
            {
            tile_u_gpu.push_back(rng.first.tile_u);
            tile_v_gpu.push_back(rng.first.tile_v);
            }
          if (do_wgridding)
            minplane_gpu.push_back(rng.first.minplane);
          }
        for (const auto &rcr: rng.second)
          {
          auto nchan = size_t(rcr.ch_end-rcr.ch_begin);
          size_t curpos=0;
          while (curpos+chunksize-isamp<=nchan)
            {
            blocklimits.push_back(row_gpu.size());
            blockstartidx.push_back(blockstartidx.back()+chunksize);
            if (store_tiles)
              {
              tile_u_gpu.push_back(rng.first.tile_u);
              tile_v_gpu.push_back(rng.first.tile_v);
              }
            if (do_wgridding)
              minplane_gpu.push_back(rng.first.minplane);
            curpos += chunksize-isamp;
            isamp = 0;
            }
          isamp += nchan-curpos;
          row_gpu.push_back(rcr.row);
          chbegin_gpu.push_back(rcr.ch_begin);
          vissum_gpu.push_back(accum);
          accum += nchan;
          }
        }
      blocklimits.push_back(row_gpu.size());
      blockstartidx.push_back(accum);
      vissum_gpu.push_back(accum);
      }
  };
class IndexComputer: public IndexComputer0
  {
  public:
    sycl::buffer<uint32_t, 1> buf_row;
    sycl::buffer<uint16_t, 1> buf_chbegin;
    sycl::buffer<uint32_t, 1> buf_vissum;
    sycl::buffer<uint32_t, 1> buf_blocklimits;
    sycl::buffer<uint32_t, 1> buf_blockstartidx;
    sycl::buffer<uint16_t, 1> buf_tileu;
    sycl::buffer<uint16_t, 1> buf_tilev;
    sycl::buffer<uint16_t, 1> buf_minplane;

    IndexComputer(const VVR &ranges, bool do_wgridding, bool store_tiles_)
      : IndexComputer0(ranges, do_wgridding, store_tiles_),
        buf_row(make_sycl_buffer(this->row_gpu)),
        buf_chbegin(make_sycl_buffer(this->chbegin_gpu)),
        buf_vissum(make_sycl_buffer(this->vissum_gpu)),
        buf_blocklimits(make_sycl_buffer(this->blocklimits)),
        buf_blockstartidx(make_sycl_buffer(this->blockstartidx)),
        buf_tileu(make_sycl_buffer(this->tile_u_gpu)),
        buf_tilev(make_sycl_buffer(this->tile_v_gpu)),
        buf_minplane(make_sycl_buffer(this->minplane_gpu))
        {}
  };

class RowchanComputer
  {
  protected:
    sycl::accessor<uint32_t,1,sycl::access::mode::read> acc_blocklimits;
    sycl::accessor<uint32_t,1,sycl::access::mode::read> acc_blockstartidx;
    sycl::accessor<uint32_t,1,sycl::access::mode::read> acc_vissum;
    sycl::accessor<uint32_t,1,sycl::access::mode::read> acc_row;
    sycl::accessor<uint16_t,1,sycl::access::mode::read> acc_chbegin;

  public:
    RowchanComputer(IndexComputer &idxcomp, sycl::handler &cgh)
      : acc_blocklimits(idxcomp.buf_blocklimits.template get_access<sycl::access::mode::read>(cgh)),
        acc_blockstartidx(idxcomp.buf_blockstartidx.template get_access<sycl::access::mode::read>(cgh)),
        acc_vissum(idxcomp.buf_vissum.template get_access<sycl::access::mode::read>(cgh)),
        acc_row(idxcomp.buf_row.template get_access<sycl::access::mode::read>(cgh)),
        acc_chbegin(idxcomp.buf_chbegin.template get_access<sycl::access::mode::read>(cgh))
      {}

    void getRowChan(size_t iblock, size_t iwork, size_t &irow, size_t &ichan) const
      {
      auto xlo = acc_blocklimits[iblock];
      auto xhi = acc_blocklimits[iblock+1];
      auto wanted = acc_blockstartidx[iblock]+iwork;
      if (wanted>=acc_blockstartidx[iblock+1])
        { irow = ~size_t(0); return; }  // nothing to do for this item
      while (xlo+1<xhi)  // bisection search
        {
        auto xmid = (xlo+xhi)/2;
        (acc_vissum[xmid]<=wanted) ? xlo=xmid : xhi=xmid;
        }
      if (acc_vissum[xhi]<=wanted)
        xlo = xhi;
      irow = acc_row[xlo];
      ichan = acc_chbegin[xlo] + (wanted-acc_vissum[xlo]);
      }
  };

template<typename T> class KernelComputer
  {
  protected:
    sycl::accessor<T,1,sycl::access::mode::read> acc_coeff;
    size_t supp, D;

  public:
    KernelComputer(sycl::buffer<T,1> &buf_coeff, size_t supp_, sycl::handler &cgh)
      : acc_coeff(buf_coeff.template get_access<sycl::access::mode::read>(cgh)),
        supp(supp_), D(supp_+3) {}
    inline void compute_uv(T ufrac, T vfrac, array<T,16> &ku, array<T,16> &kv) const
      {
      auto x0 = T(ufrac)*T(-2)+T(supp-1);
      auto y0 = T(vfrac)*T(-2)+T(supp-1);
      for (size_t i=0; i<supp; ++i)
        {
        Tcalc resu=acc_coeff[i], resv=acc_coeff[i];
        for (size_t j=1; j<=D; ++j)
          {
          resu = resu*x0 + acc_coeff[j*supp+i];
          resv = resv*y0 + acc_coeff[j*supp+i];
          }
        ku[i] = resu;
        kv[i] = resv;
        }
      }
    inline void compute_uvw(T ufrac, T vfrac, T wval, size_t nth, array<T,16> &ku, array<T,16> &kv) const
      {
      auto x0 = T(ufrac)*T(-2)+T(supp-1);
      auto y0 = T(vfrac)*T(-2)+T(supp-1);
      auto z0 = T(wval-nth)*T(2)+T(supp-1);
      Tcalc resw=acc_coeff[nth];
      for (size_t j=1; j<=D; ++j)
        resw = resw*z0 + acc_coeff[j*supp+nth];
      for (size_t i=0; i<supp; ++i)
        {
        Tcalc resu=acc_coeff[i], resv=acc_coeff[i];
        for (size_t j=1; j<=D; ++j)
          {
          resu = resu*x0 + acc_coeff[j*supp+i];
          resv = resv*y0 + acc_coeff[j*supp+i];
          }
        ku[i] = resu*resw;
        kv[i] = resv;
        }
      }
  };

class CoordCalculator
  {
  private:
    size_t nu, nv;
    int maxiu0, maxiv0;
    double pixsize_x, pixsize_y, ushift, vshift;

  public:
    CoordCalculator (size_t nu_, size_t nv_, int maxiu0_, int maxiv0_, double pixsize_x_, double pixsize_y_, double ushift_, double vshift_)
      : nu(nu_), nv(nv_), maxiu0(maxiu0_), maxiv0(maxiv0_), pixsize_x(pixsize_x_), pixsize_y(pixsize_y_), ushift(ushift_), vshift(vshift_) {}

    [[gnu::always_inline]] void getpix(double u_in, double v_in, double &u, double &v, int &iu0, int &iv0) const
      {
      u = u_in*pixsize_x;
      u = (u-floor(u))*nu;
      iu0 = std::min(int(u+ushift)-int(nu), maxiu0);
      u -= iu0;
      v = v_in*pixsize_y;
      v = (v-floor(v))*nv;
      iv0 = std::min(int(v+vshift)-int(nv), maxiv0);
      v -= iv0;
      }
  };

    void dirty2x_gpu()
      {
      if (do_wgridding)
        {
#if (defined(DUCC0_HAVE_SYCL))
timers.push("GPU degridding");
          
        { // Device buffer scope
        sycl::queue q{sycl::default_selector()};
        // dirty image
        MR_assert(dirty_in.contiguous(), "dirty image is not contiguous");
  
        auto bufdirty(make_sycl_buffer(dirty_in));
        // grid (only on GPU)
        sycl::buffer<complex<Tcalc>, 2> bufgrid{sycl::range<2>(nu,nv)};

        Baselines_GPU_prep bl_prep(bl);
        
        auto bufvis(make_sycl_buffer(ms_out));
        const auto &dcoef(krn->Coeff());
        vector<Tcalc> coef(dcoef.size());
        for (size_t i=0;i<coef.size(); ++i) coef[i] = Tcalc(dcoef[i]);
        auto bufcoef(make_sycl_buffer(coef));

        sycl_zero_buffer(q, bufvis);
        sycl_zero_buffer(q, bufgrid);

        auto cfu = krn->corfunc(nxdirty/2+1, 1./nu, nthreads);
        auto cfv = krn->corfunc(nydirty/2+1, 1./nv, nthreads);
  // FIXME: cast to Timg
        auto bufcfu(make_sycl_buffer(cfu));
        auto bufcfv(make_sycl_buffer(cfv));

        // build index structure
        timers.push("index creation");
#ifdef BUFFERING
        IndexComputer idxcomp(ranges, do_wgridding, true);
#else
        IndexComputer idxcomp(ranges, do_wgridding, false);
#endif
        timers.pop();

        // applying correction to dirty image on GPU

        q.submit([&](sycl::handler &cgh)
          {
#if 0
for(auto i=lo; i<hi; ++i)
  {
  double fx = sqr(x0+i*pixsize_x);
  for (size_t j=0; j<nyd; ++j)
    {
    double fy = sqr(y0+j*pixsize_y);
    double fct = 0;
    auto tmp = 1-fx-fy;
    if (tmp>=0)
      {
      auto nm1 = (-fx-fy)/(sqrt(tmp)+1); // accurate form of sqrt(1-x-y)-1
      fct = krn->corfunc((nm1+nshift)*dw);
      if (divide_by_n)
        fct /= nm1+1;
      }
    else // beyond the horizon, don't really know what to do here
      fct = divide_by_n ? 0 : krn->corfunc((sqrt(-tmp)-1)*dw);
    auto i2=min(i, nxdirty-i), j2=min(j, nydirty-j);
    fct *= cfu[nxdirty/2-i2]*cfv[nydirty/2-j2];
    dirty(i,j)*=Timg(fct);
    }
  }
#endif
          auto accdirty{bufdirty.template get_access<sycl::access::mode::read_write>(cgh)};
          auto acccfu{bufcfu.template get_access<sycl::access::mode::read>(cgh)};
          auto acccfv{bufcfv.template get_access<sycl::access::mode::read>(cgh)};
          double x0 = lshift-0.5*nxdirty*pixsize_x,
                 y0 = mshift-0.5*nydirty*pixsize_y;
          cgh.parallel_for(sycl::range<2>(nxdirty, nydirty), [nxdirty=nxdirty,nydirty=nydirty,accdirty,acccfu,acccfv,pixsize_x=pixsize_x,pixsize_y=pixsize_y,x0,y0,divide_by_n=divide_by_n](sycl::item<2> item)
            {
            auto i = item.get_id(0);
            auto j = item.get_id(1);
            double fx = sqr(x0+i*pixsize_x);
            double fy = sqr(y0+j*pixsize_y);
            double fct = 0;
            auto tmp = 1-fx-fy;
            if (tmp>=0)
              {
              auto nm1 = (-fx-fy)/(sqrt(tmp)+1); // accurate form of sqrt(1-x-y)-1
// FIXME: still need code to compute correction function on GPU
//              fct = krn->corfunc((nm1+nshift)*dw);
              if (divide_by_n)
                fct /= nm1+1;
              }
     //       else // beyond the horizon, don't really know what to do here
     //         fct = divide_by_n ? 0 : krn->corfunc((sqrt(-tmp)-1)*dw);

            int icfu = abs(int(nxdirty/2)-int(i));
            int icfv = abs(int(nydirty/2)-int(j));
            auto fctu = acccfu[icfu];
            auto fctv = acccfv[icfv];
            accdirty[i][j]*=Tcalc(fctu*fctv);
            });
          });

        for (size_t pl=0; pl<nplanes; ++pl)
          {
cout << "plane: " << pl << endl;
          double w = wmin+pl*dw;

          // zeroing grid
          q.submit([&](sycl::handler &cgh)
            {
            auto accgrid{bufgrid.template get_access<sycl::access::mode::discard_write>(cgh)};
            cgh.parallel_for(sycl::range<2>(nu, nv), [=](sycl::item<2> item)
              { accgrid[item.get_id(0)][item.get_id(1)] = Timg(0); });
            });

          // copying to grid and applying wscreen
          q.submit([&](sycl::handler &cgh)
            {
            auto accdirty{bufdirty.template get_access<sycl::access::mode::read>(cgh)};
            auto accgrid{bufgrid.template get_access<sycl::access::mode::write>(cgh)};
            double x0 = lshift-0.5*nxdirty*pixsize_x,
                   y0 = mshift-0.5*nydirty*pixsize_y;
            cgh.parallel_for(sycl::range<2>(nxdirty, nydirty), [nxdirty=nxdirty, nydirty=nydirty, nu=nu, nv=nv, pixsize_x=pixsize_x, pixsize_y=pixsize_y,nshift=nshift,accgrid,accdirty,x0,y0,w](sycl::item<2> item)
              {
              auto i = item.get_id(0);
              auto j = item.get_id(1);
              size_t i2 = nu-nxdirty/2+i;
              if (i2>=nu) i2-=nu;
              size_t j2 = nv-nydirty/2+j;
              if (j2>=nv) j2-=nv;
              double fx = sqr(x0+i*pixsize_x);
              double fy = sqr(y0+j*pixsize_y);
              double myphase = phase(fx, fy, w, false, nshift);
              accgrid[i2][j2] = complex<Tcalc>(polar(1., myphase))*accdirty[i][j];
              });
            });

          // FFT
          sycl_c2c(q, bufgrid, true);

#ifdef BUFFERING
          constexpr size_t blksz = 1024;
          for (size_t blockofs=0; blockofs<idxcomp.blocklimits.size()-1; blockofs+=blksz)
            {
            size_t blockend = min(blockofs+blksz,idxcomp.blocklimits.size()-1);
#endif
          q.submit([&](sycl::handler &cgh)
            {
            Baselines_GPU blloc(bl_prep, cgh);
            KernelComputer<Tcalc> kcomp(bufcoef, supp, cgh);
            CoordCalculator ccalc(nu, nv, maxiu0, maxiv0, pixsize_x, pixsize_y, ushift,vshift);
            RowchanComputer rccomp(idxcomp,cgh);
  
#ifdef BUFFERING
            auto acc_tileu{idxcomp.buf_tileu.template get_access<sycl::access::mode::read>(cgh)};
            auto acc_tilev{idxcomp.buf_tilev.template get_access<sycl::access::mode::read>(cgh)};
#endif
            auto acc_minplane{idxcomp.buf_minplane.template get_access<sycl::access::mode::read>(cgh)};
            auto accgrid{bufgrid.template get_access<sycl::access::mode::read>(cgh)};
            auto accvis{bufvis.template get_access<sycl::access::mode::write>(cgh)};
#ifdef BUFFERING
            sycl::range<2> global(blockend-blockofs, idxcomp.chunksize);
            sycl::range<2> local(1, idxcomp.chunksize);
            int nsafe = (supp+1)/2;
            size_t sidelen = 2*nsafe+(1<<logsquare);
#ifndef __INTEL_LLVM_COMPILER
            sycl::local_accessor<complex<Tcalc>,2> tile({sidelen,sidelen}, cgh);
#else
            sycl::accessor<complex<Tcalc>,2,sycl::access::mode::read_write, sycl::access::target::local> tile({sidelen,sidelen}, cgh);
#endif
            cgh.parallel_for(sycl::nd_range(global,local), [accgrid,accvis,nu=nu,nv=nv,supp=supp,shifting=shifting,lshift=lshift,mshift=mshift,rccomp,blloc,ccalc,kcomp,pl,acc_minplane,blockofs,sidelen,nsafe,acc_tileu,acc_tilev,tile,w,dw=dw](sycl::nd_item<2> item)
#else
            cgh.parallel_for(sycl::range<2>(idxcomp.blocklimits.size()-1, idxcomp.chunksize), [accgrid,accvis,nu=nu,nv=nv,supp=supp,shifting=shifting,lshift=lshift,mshift=mshift,rccomp,blloc,ccalc,kcomp,pl,acc_minplane,w,dw=dw](sycl::item<2> item)
#endif
              {
#ifdef BUFFERING
              auto iblock = item.get_global_id(0)+blockofs;
              auto iwork = item.get_local_id(1);
              auto minplane = acc_minplane[iblock];
              if ((pl<minplane) || (pl>=minplane+supp))  // plane not in range
                return;
              // preparation
              auto u_tile = acc_tileu[iblock];
              auto v_tile = acc_tilev[iblock];

              //size_t ofs = (supp-1)/2;
              for (size_t i=iwork; i<sidelen*sidelen; i+=item.get_local_range(1))
                {
                size_t iu = i/sidelen, iv = i%sidelen;
                tile[iu][iv] = accgrid[(iu+u_tile*(1<<logsquare)+nu-nsafe)%nu][(iv+v_tile*(1<<logsquare)+nv-nsafe)%nv];
                }
              item.barrier();
#else
              auto iblock = item.get_id(0);
              auto iwork = item.get_id(1);
              auto minplane = acc_minplane[iblock];
              if ((pl<minplane) || (pl>=minplane+supp))  // plane not in range
                return;
#endif
  
              size_t irow, ichan;
              rccomp.getRowChan(iblock, iwork, irow, ichan);
              if (irow==~size_t(0)) return;
  
              auto coord = blloc.effectiveCoord(irow, ichan);
              auto imflip = coord.FixW();
  
              // compute fractional and integer indices in "grid"
              double ufrac,vfrac;
              int iu0, iv0;
              ccalc.getpix( coord.u, coord.v, ufrac, vfrac, iu0, iv0);
  
              // compute kernel values
              array<Tcalc, 16> ukrn, vkrn;
size_t nth=pl-minplane;
auto wval=Tcalc((coord.w-w)/dw);
              kcomp.compute_uvw(ufrac, vfrac, wval, nth, ukrn, vkrn);
  
              // loop over supp*supp pixels from "grid"
              complex<Tcalc> res=0;
#ifdef BUFFERING
              int bu0=((((iu0+nsafe)>>logsquare)<<logsquare))-nsafe;
              int bv0=((((iv0+nsafe)>>logsquare)<<logsquare))-nsafe;
              for (size_t i=0; i<supp; ++i)
                {
                complex<Tcalc> tmp = 0;
                for (size_t j=0; j<supp; ++j)
                  tmp += vkrn[j]*tile[iu0-bu0+i][iv0-bv0+j];
                res += ukrn[i]*tmp;
                }
#else
              auto iustart=size_t((iu0+nu)%nu);
              auto ivstart=size_t((iv0+nv)%nv);
              for (size_t i=0, realiu=iustart; i<supp;
                   ++i, realiu = (realiu+1<nu)?realiu+1 : 0)
                {
                complex<Tcalc> tmp = 0;
                for (size_t j=0, realiv=ivstart; j<supp;
                     ++j, realiv = (realiv+1<nv)?realiv+1 : 0)
                  tmp += vkrn[j]*accgrid[realiu][realiv];
                res += ukrn[i]*tmp;
                }
#endif
              res.imag(res.imag()*imflip);
  
              if (shifting)
                {
                // apply phase
                double fct = coord.u*lshift + coord.v*mshift;
                if constexpr (is_same<double, Tcalc>::value)
                  fct*=twopi;
                else
                  // we are reducing accuracy,
                  // so let's better do range reduction first
                  fct = twopi*(fct-floor(fct));
                complex<Tcalc> phase(cos(Tcalc(fct)), -imflip*sin(Tcalc(fct)));
                res *= phase;
                }
              accvis[irow][ichan] += res;
              });
            });
#ifdef BUFFERING
}
#endif
          } // end of loop over planes
        }  // end of device buffer scope, buffers are written back
        timers.poppush("weight application");
        if (wgt.stride(0)!=0)  // we need to apply weights!
          execParallel(bl.Nrows(), nthreads, [&](size_t lo, size_t hi)
            {
            auto nchan = bl.Nchannels();
            for (auto irow=lo; irow<hi; ++irow)
              for (size_t ichan=0; ichan<nchan; ++ichan)
                ms_out(irow, ichan) *= wgt(irow, ichan);
            });
        timers.pop();
#else
        MR_fail("CUDA not found");
#endif
        }
      else
        {
#if (defined(DUCC0_HAVE_SYCL))
timers.push("GPU degridding");
        { // Device buffer scope
        sycl::queue q{sycl::default_selector()};
        // dirty image
        MR_assert(dirty_in.contiguous(), "dirty image is not contiguous");

        auto bufdirty(make_sycl_buffer(dirty_in));
        // grid (only on GPU)
        sycl::buffer<complex<Tcalc>, 2> bufgrid{sycl::range<2>(nu,nv)};

        Baselines_GPU_prep bl_prep(bl);
        auto bufvis(make_sycl_buffer(ms_out));
        const auto &dcoef(krn->Coeff());
        vector<Tcalc> coef(dcoef.size());
        for (size_t i=0;i<coef.size(); ++i) coef[i] = Tcalc(dcoef[i]);
        auto bufcoef(make_sycl_buffer(coef));

        sycl_zero_buffer(q, bufvis);
        sycl_zero_buffer(q, bufgrid);
        auto cfu = krn->corfunc(nxdirty/2+1, 1./nu, nthreads);
        auto cfv = krn->corfunc(nydirty/2+1, 1./nv, nthreads);
// FIXME: cast to Timg
        auto bufcfu(make_sycl_buffer(cfu));
        auto bufcfv(make_sycl_buffer(cfv));
        // copying to grid and applying correction
        q.submit([&](sycl::handler &cgh)
          {
          auto accdirty{bufdirty.template get_access<sycl::access::mode::read>(cgh)};
          auto acccfu{bufcfu.template get_access<sycl::access::mode::read>(cgh)};
          auto acccfv{bufcfv.template get_access<sycl::access::mode::read>(cgh)};
          auto accgrid{bufgrid.template get_access<sycl::access::mode::write>(cgh)};
          cgh.parallel_for(sycl::range<2>(nxdirty, nydirty), [accdirty,acccfu,acccfv,accgrid,nxdirty=nxdirty,nydirty=nydirty,nu=nu,nv=nv](sycl::item<2> item)
            {
            auto i = item.get_id(0);
            auto j = item.get_id(1);
            int icfu = abs(int(nxdirty/2)-int(i));
            int icfv = abs(int(nydirty/2)-int(j));
            size_t i2 = nu-nxdirty/2+i;
            if (i2>=nu) i2-=nu;
            size_t j2 = nv-nydirty/2+j;
            if (j2>=nv) j2-=nv;
            auto fctu = acccfu[icfu];
            auto fctv = acccfv[icfv];
            accgrid[i2][j2] = accdirty[i][j]*Tcalc(fctu*fctv);
            });
          });

        // FFT
        sycl_c2c(q, bufgrid, true);

        // build index structure
        timers.push("index creation");
#ifdef BUFFERING
        IndexComputer idxcomp(ranges, do_wgridding, true);
#else
        IndexComputer idxcomp(ranges, do_wgridding, false);
#endif
        timers.pop();

#ifdef BUFFERING
        constexpr size_t blksz = 1024;
        for (size_t blockofs=0; blockofs<idxcomp.blocklimits.size()-1; blockofs+=blksz)
          {
          size_t blockend = min(blockofs+blksz,idxcomp.blocklimits.size()-1);
#endif
        q.submit([&](sycl::handler &cgh)
          {
          Baselines_GPU blloc(bl_prep, cgh);
          KernelComputer<Tcalc> kcomp(bufcoef, supp, cgh);
          CoordCalculator ccalc(nu, nv, maxiu0, maxiv0, pixsize_x, pixsize_y, ushift,vshift);
          RowchanComputer rccomp(idxcomp, cgh);

#ifdef BUFFERING
          auto acc_tileu{idxcomp.buf_tileu.template get_access<sycl::access::mode::read>(cgh)};
          auto acc_tilev{idxcomp.buf_tilev.template get_access<sycl::access::mode::read>(cgh)};
#endif
          auto accgrid{bufgrid.template get_access<sycl::access::mode::read>(cgh)};
          auto accvis{bufvis.template get_access<sycl::access::mode::write>(cgh)};
          auto degree = krn->degree();
#ifdef BUFFERING
          sycl::range<2> global(blockend-blockofs, idxcomp.chunksize);
          sycl::range<2> local(1, idxcomp.chunksize);
          int nsafe = (supp+1)/2;
          size_t sidelen = 2*nsafe+(1<<logsquare);
#ifndef __INTEL_LLVM_COMPILER
          sycl::local_accessor<complex<Tcalc>,2> tile({sidelen,sidelen}, cgh);
#else
          sycl::accessor<complex<Tcalc>,2,sycl::access::mode::read_write, sycl::access::target::local> tile({sidelen,sidelen}, cgh);
#endif
          cgh.parallel_for(sycl::nd_range(global,local), [accgrid,accvis,acc_tileu,acc_tilev,tile,nu=nu,nv=nv,supp=supp,shifting=shifting,lshift=lshift,mshift=mshift,rccomp,blloc,ccalc,kcomp,blockofs,nsafe,sidelen](sycl::nd_item<2> item)
#else
          cgh.parallel_for(sycl::range<2>(idxcomp.blocklimits.size()-1, idxcomp.chunksize), [accgrid,accvis,nu=nu,nv=nv,supp=supp,shifting=shifting,lshift=lshift,mshift=mshift,rccomp,blloc,ccalc,kcomp](sycl::item<2> item)
#endif
            {
#ifdef BUFFERING
            auto iblock = item.get_global_id(0)+blockofs;
            auto iwork = item.get_local_id(1);
            // preparation
            auto u_tile = acc_tileu[iblock];
            auto v_tile = acc_tilev[iblock];
            //size_t ofs = (supp-1)/2;
            for (size_t i=iwork; i<sidelen*sidelen; i+=item.get_local_range(1))
              {
              size_t iu = i/sidelen, iv = i%sidelen;
              tile[iu][iv] = accgrid[(iu+u_tile*(1<<logsquare)+nu-nsafe)%nu][(iv+v_tile*(1<<logsquare)+nv-nsafe)%nv];
              }
            item.barrier();
#else
            auto iblock = item.get_id(0);
            auto iwork = item.get_id(1);
#endif

            size_t irow, ichan;
            rccomp.getRowChan(iblock, iwork, irow, ichan);
            if (irow==~size_t(0)) return;

            auto coord = blloc.effectiveCoord(irow, ichan);
            auto imflip = coord.FixW();

            // compute fractional and integer indices in "grid"
            double ufrac,vfrac;
            int iu0, iv0;
            ccalc.getpix( coord.u, coord.v, ufrac, vfrac, iu0, iv0);

            // compute kernel values
            array<Tcalc, 16> ukrn, vkrn;
            kcomp.compute_uv(ufrac, vfrac, ukrn, vkrn);

            // loop over supp*supp pixels from "grid"
            complex<Tcalc> res=0;
#ifdef BUFFERING
            int bu0=((((iu0+nsafe)>>logsquare)<<logsquare))-nsafe;
            int bv0=((((iv0+nsafe)>>logsquare)<<logsquare))-nsafe;
            for (size_t i=0; i<supp; ++i)
              {
              complex<Tcalc> tmp = 0;
              for (size_t j=0; j<supp; ++j)
                tmp += vkrn[j]*tile[iu0-bu0+i][iv0-bv0+j];
              res += ukrn[i]*tmp;
              }
#else
            auto iustart=size_t((iu0+nu)%nu);
            auto ivstart=size_t((iv0+nv)%nv);
            for (size_t i=0, realiu=iustart; i<supp;
                 ++i, realiu = (realiu+1<nu)?realiu+1 : 0)
              {
              complex<Tcalc> tmp = 0;
              for (size_t j=0, realiv=ivstart; j<supp;
                   ++j, realiv = (realiv+1<nv)?realiv+1 : 0)
                tmp += vkrn[j]*accgrid[realiu][realiv];
              res += ukrn[i]*tmp;
              }
#endif
            res.imag(res.imag()*imflip);

            if (shifting)
              {
              // apply phase
              double fct = coord.u*lshift + coord.v*mshift;
              if constexpr (is_same<double, Tcalc>::value)
                fct*=twopi;
              else
                // we are reducing accuracy,
                // so let's better do range reduction first
                fct = twopi*(fct-floor(fct));
              complex<Tcalc> phase(cos(Tcalc(fct)), -imflip*sin(Tcalc(fct)));
              res *= phase;
              }
            accvis[irow][ichan] = res;
            });
          });
#ifdef BUFFERING
}
#endif
        }  // end of device buffer scope, buffers are written back
        timers.poppush("weight application");
        if (wgt.stride(0)!=0)  // we need to apply weights!
          execParallel(bl.Nrows(), nthreads, [&](size_t lo, size_t hi)
            {
            auto nchan = bl.Nchannels();
            for (auto irow=lo; irow<hi; ++irow)
              for (size_t ichan=0; ichan<nchan; ++ichan)
                ms_out(irow, ichan) *= wgt(irow, ichan);
            });
        timers.pop();
#else
        MR_fail("CUDA not found");
#endif
        }
      }



template<typename T> static void atomic_add(complex<T> &a, const complex<T> &b)
  {
  T *aptr = reinterpret_cast<T *>(&a);
#ifndef __INTEL_LLVM_COMPILER
  sycl::atomic_ref<T, sycl::memory_order::relaxed, sycl::memory_scope::device> re(aptr[0]);
  re += b.real();
  sycl::atomic_ref<T, sycl::memory_order::relaxed, sycl::memory_scope::device> im(aptr[1]);
  im += b.imag();
#else
  sycl::ext::oneapi::atomic_ref<T, sycl::memory_order::relaxed, sycl::memory_scope::device,sycl::access::address_space::global_space> re(aptr[0]);
  re += b.real();
  sycl::ext::oneapi::atomic_ref<T, sycl::memory_order::relaxed, sycl::memory_scope::device,sycl::access::address_space::global_space> im(aptr[1]);
  im += b.imag();
#endif
  }

    void x2dirty_gpu()
      {
      if (do_wgridding)
        MR_fail("not implemented");
      else
        {
#if (defined(DUCC0_HAVE_SYCL))
timers.push("GPU gridding");
        timers.push("weight application");
        bool do_weights = (wgt.stride(0)!=0);
        vmav<complex<Tms>,2> ms_tmp({do_weights ? bl.Nrows() : 1, do_weights ? bl.Nchannels() : 1});
        if (do_weights)
          mav_apply([](const complex<Tms> &a, const Tms &b, complex<Tms> &c)
            { c = a*b; }, nthreads, ms_in, wgt, ms_tmp);
        const cmav<complex<Tms>,2> &ms(do_weights ? ms_tmp : ms_in);
        timers.pop();

        { // Device buffer scope
        sycl::queue q{sycl::default_selector()};
        // dirty image
        MR_assert(dirty_out.contiguous(), "dirty image is not contiguous");
        auto bufdirty(make_sycl_buffer(dirty_out));
        // grid (only on GPU)
        sycl::buffer<complex<Tcalc>, 2> bufgrid{sycl::range<2>(nu,nv)};

        Baselines_GPU_prep bl_prep(bl);
        auto bufvis(make_sycl_buffer(ms));

        const auto &dcoef(krn->Coeff());
        vector<Tcalc> coef(dcoef.size());
        for (size_t i=0;i<coef.size(); ++i) coef[i] = Tcalc(dcoef[i]);
        auto bufcoef(make_sycl_buffer(coef));

        sycl_zero_buffer(q, bufgrid);
        auto cfu = krn->corfunc(nxdirty/2+1, 1./nu, nthreads);
        auto cfv = krn->corfunc(nydirty/2+1, 1./nv, nthreads);
// FIXME: cast to Timg
        auto bufcfu(make_sycl_buffer(cfu));
        auto bufcfv(make_sycl_buffer(cfv));

        // build index structure
        timers.push("index creation");
        IndexComputer idxcomp(ranges, do_wgridding, true);
        timers.pop();

        constexpr size_t blksz = 1024;
        for (size_t blockofs=0; blockofs<idxcomp.blocklimits.size()-1; blockofs+=blksz)
          {
          size_t blockend = min(blockofs+blksz,idxcomp.blocklimits.size()-1);
          q.submit([&](sycl::handler &cgh)
            {
            Baselines_GPU blloc(bl_prep, cgh);
            KernelComputer<Tcalc> kcomp(bufcoef, supp, cgh);
            CoordCalculator ccalc(nu, nv, maxiu0, maxiv0, pixsize_x, pixsize_y, ushift,vshift);
            RowchanComputer rccomp(idxcomp, cgh);

            auto acc_tileu{idxcomp.buf_tileu.template get_access<sycl::access::mode::read>(cgh)};
            auto acc_tilev{idxcomp.buf_tilev.template get_access<sycl::access::mode::read>(cgh)};

            auto accgrid{bufgrid.template get_access<sycl::access::mode::write>(cgh)};
            auto accvis{bufvis.template get_access<sycl::access::mode::read>(cgh)};
            auto degree = krn->degree();

            sycl::range<2> global(blockend-blockofs, idxcomp.chunksize);
            sycl::range<2> local(1, idxcomp.chunksize);
            int nsafe = (supp+1)/2;
            size_t sidelen = 2*nsafe+(1<<logsquare);
#ifndef __INTEL_LLVM_COMPILER
            sycl::local_accessor<complex<Tcalc>,2> tile({sidelen,sidelen}, cgh);
#else
            sycl::accessor<complex<Tcalc>,2,sycl::access::mode::read_write, sycl::access::target::local> tile({sidelen,sidelen}, cgh);
#endif
            cgh.parallel_for(sycl::nd_range(global,local), [accgrid,accvis,acc_tileu,acc_tilev,tile,nu=nu,nv=nv,supp=supp,shifting=shifting,lshift=lshift,mshift=mshift,rccomp,blloc,ccalc,kcomp,blockofs,nsafe,sidelen](sycl::nd_item<2> item)
              {
              auto iblock = item.get_global_id(0)+blockofs;
              auto iwork = item.get_local_id(1);

              // preparation
              // zero local buffer (FIXME is this needed?)
              for (size_t i=iwork; i<sidelen*sidelen; i+=item.get_local_range(1))
                {
                size_t iu = i/sidelen, iv = i%sidelen;
                tile[iu][iv] = Tcalc(0);
                }
              item.barrier();

              size_t irow, ichan;
              rccomp.getRowChan(iblock, iwork, irow, ichan);
              if (irow!=~size_t(0))
                {
                auto coord = blloc.effectiveCoord(irow, ichan);
                auto imflip = coord.FixW();

                // compute fractional and integer indices in "grid"
                double ufrac,vfrac;
                int iu0, iv0;
                ccalc.getpix( coord.u, coord.v, ufrac, vfrac, iu0, iv0);

                // compute kernel values
                array<Tcalc, 16> ukrn, vkrn;
                kcomp.compute_uv(ufrac, vfrac, ukrn, vkrn);

                // loop over supp*supp pixels from "grid"
                complex<Tcalc> val=accvis[irow][ichan];
                if (shifting)
                  {
                  // apply phase
                  double fct = coord.u*lshift + coord.v*mshift;
                  if constexpr (is_same<double, Tcalc>::value)
                    fct*=twopi;
                  else
                    // we are reducing accuracy,
                    // so let's better do range reduction first
                    fct = twopi*(fct-floor(fct));
                  complex<Tcalc> phase(cos(Tcalc(fct)), imflip*sin(Tcalc(fct)));
                  val *= phase;
                  }
                val.imag(val.imag()*imflip);

                int bu0=((((iu0+nsafe)>>logsquare)<<logsquare))-nsafe;
                int bv0=((((iv0+nsafe)>>logsquare)<<logsquare))-nsafe;
                for (size_t i=0; i<supp; ++i)
                  {
                  auto tmp = ukrn[i]*val;
                  for (size_t j=0; j<supp; ++j)
                    atomic_add(tile[iu0-bu0+i][iv0-bv0+j], vkrn[j]*tmp);
                  }
                }

              // add local buffer back to global buffer
              auto u_tile = acc_tileu[iblock];
              auto v_tile = acc_tilev[iblock];
              item.barrier();
              //size_t ofs = (supp-1)/2;
              for (size_t i=iwork; i<sidelen*sidelen; i+=item.get_local_range(1))
                {
                size_t iu = i/sidelen, iv = i%sidelen;
                atomic_add(accgrid[(iu+u_tile*(1<<logsquare)+nu-nsafe)%nu][(iv+v_tile*(1<<logsquare)+nv-nsafe)%nv], tile[iu][iv]);
                }
              });
            });
          }
        // FFT
        sycl_c2c(q, bufgrid, false);  // FIXME normalization?

        // copying to dirty image and applying correction
        q.submit([&](sycl::handler &cgh)
          {
          auto accdirty{bufdirty.template get_access<sycl::access::mode::discard_write>(cgh)};
          auto acccfu{bufcfu.template get_access<sycl::access::mode::read>(cgh)};
          auto acccfv{bufcfv.template get_access<sycl::access::mode::read>(cgh)};
          auto accgrid{bufgrid.template get_access<sycl::access::mode::read>(cgh)};
          cgh.parallel_for(sycl::range<2>(nxdirty, nydirty), [accdirty,acccfu,acccfv,accgrid,nxdirty=nxdirty,nydirty=nydirty,nu=nu,nv=nv](sycl::item<2> item)
            {
            auto i = item.get_id(0);
            auto j = item.get_id(1);
            int icfu = abs(int(nxdirty/2)-int(i));
            int icfv = abs(int(nydirty/2)-int(j));
            size_t i2 = nu-nxdirty/2+i;
            if (i2>=nu) i2-=nu;
            size_t j2 = nv-nydirty/2+j;
            if (j2>=nv) j2-=nv;
            auto fctu = acccfu[icfu];
            auto fctv = acccfv[icfv];
            accdirty[i][j] = (accgrid[i2][j2]*Tcalc(fctu*fctv)).real();
            });
          });
        }  // end of device buffer scope, buffers are written back
        timers.pop();
#else
        MR_fail("CUDA not found");
#endif
        }
      }

    auto getNuNv()
      {
      timers.push("parameter calculation");

      double xmin = lshift - 0.5*nxdirty*pixsize_x,
             xmax = xmin + (nxdirty-1)*pixsize_x,
             ymin = mshift - 0.5*nydirty*pixsize_y,
             ymax = ymin + (nydirty-1)*pixsize_y;
      vector<double> xext{xmin, xmax},
                     yext{ymin, ymax};
      if (xmin*xmax<0) xext.push_back(0);
      if (ymin*ymax<0) yext.push_back(0);
      nm1min = 1e300, nm1max = -1e300;
      for (auto xc: xext)
        for (auto yc: yext)
          {
          double tmp = xc*xc+yc*yc;
          double nval;
          if (tmp <= 1.) // northern hemisphere
            nval = sqrt(1.-tmp) - 1.;
          else
            nval = -sqrt(tmp-1.) -1.;
          nm1min = min(nm1min, nval);
          nm1max = max(nm1max, nval);
          }
      nshift = (no_nshift||(!do_wgridding)) ? 0. : -0.5*(nm1max+nm1min);
      shifting = lmshift || (nshift!=0);

      auto idx = getAvailableKernels<Tcalc>(epsilon, sigma_min, sigma_max);
      double mincost = 1e300;
      constexpr double nref_fft=2048;
      constexpr double costref_fft=0.0693;
      size_t minnu=0, minnv=0, minidx=KernelDB.size();
      size_t vlen = gridding ? mysimd<Tacc>::size() : mysimd<Tcalc>::size();
      for (size_t i=0; i<idx.size(); ++i)
        {
        const auto &krn(KernelDB[idx[i]]);
        auto supp = krn.W;
        auto nvec = (supp+vlen-1)/vlen;
        auto ofactor = krn.ofactor;
        size_t nu=2*good_size_complex(size_t(nxdirty*ofactor*0.5)+1);
        size_t nv=2*good_size_complex(size_t(nydirty*ofactor*0.5)+1);
        double logterm = log(nu*nv)/log(nref_fft*nref_fft);
        double fftcost = nu/nref_fft*nv/nref_fft*logterm*costref_fft;
        double gridcost = 2.2e-10*nvis*(supp*nvec*vlen + ((2*nvec+1)*(supp+3)*vlen));
        if (gridding) gridcost *= sizeof(Tacc)/sizeof(Tcalc);
        if (do_wgridding)
          {
          double dw = 0.5/ofactor/max(abs(nm1max+nshift), abs(nm1min+nshift));
          size_t nplanes = size_t((wmax_d-wmin_d)/dw+supp);
          fftcost *= nplanes;
          gridcost *= supp;
          }
        // FIXME: heuristics could be improved
        gridcost /= nthreads;  // assume perfect scaling for now
        constexpr double max_fft_scaling = 6;
        constexpr double scaling_power=2;
        auto sigmoid = [](double x, double m, double s)
          {
          auto x2 = x-1;
          auto m2 = m-1;
          return 1.+x2/pow((1.+pow(x2/m2,s)),1./s);
          };
        fftcost /= sigmoid(nthreads, max_fft_scaling, scaling_power);
        double cost = fftcost+gridcost;
        if (cost<mincost)
          {
          mincost=cost;
          minnu=nu;
          minnv=nv;
          minidx = idx[i];
          }
        }
      timers.pop();
      nu = minnu;
      nv = minnv;
      return minidx;
      }

    void scanData()
      {
      timers.push("Initial scan");
      size_t nrow=bl.Nrows(),
             nchan=bl.Nchannels();
      checkShape(wgt.shape(),{nrow,nchan});
      checkShape(ms_in.shape(), {nrow,nchan});
      checkShape(mask.shape(), {nrow,nchan});

      nvis=0;
      wmin_d=1e300;
      wmax_d=-1e300;
      mutex mut;
      execParallel(nrow, nthreads, [&](size_t lo, size_t hi)
        {
        double lwmin_d=1e300, lwmax_d=-1e300;
        size_t lnvis=0;
        for(auto irow=lo; irow<hi; ++irow)
          for (size_t ichan=0, idx=irow*nchan; ichan<nchan; ++ichan, ++idx)
//            if (mask(irow,ichan) && (wgt(irow, ichan)!=0) && (norm(ms_in(irow,ichan)!=0)))
            if (norm(ms_in(irow,ichan))*wgt(irow,ichan)*mask(irow,ichan) != 0)
              {
              ++lnvis;
              double w = bl.absEffectiveW(irow, ichan);
              lwmin_d = min(lwmin_d, w);
              lwmax_d = max(lwmax_d, w);
              }
        {
        lock_guard<mutex> lock(mut);
        wmin_d = min(wmin_d, lwmin_d);
        wmax_d = max(wmax_d, lwmax_d);
        nvis += lnvis;
        }
        });
      timers.pop();
      }

  public:
    Params(const cmav<double,2> &uvw, const cmav<double,1> &freq,
           const cmav<complex<Tms>,2> &ms_in_, vmav<complex<Tms>,2> &ms_out_,
           const cmav<Timg,2> &dirty_in_, vmav<Timg,2> &dirty_out_,
           const cmav<Tms,2> &wgt_, const cmav<uint8_t,2> &mask_,
           double pixsize_x_, double pixsize_y_, double epsilon_,
           bool do_wgridding_, size_t nthreads_, size_t verbosity_,
           bool negate_v_, bool divide_by_n_, double sigma_min_,
           double sigma_max_, double center_x, double center_y, bool allow_nshift,
           bool gpu_)
      : gridding(ms_out_.size()==0),
        timers(gridding ? "gridding" : "degridding"),
        ms_in(ms_in_), ms_out(ms_out_),
        dirty_in(dirty_in_), dirty_out(dirty_out_),
        wgt(wgt_), mask(mask_),
        pixsize_x(pixsize_x_), pixsize_y(pixsize_y_),
        nxdirty(gridding ? dirty_out.shape(0) : dirty_in.shape(0)),
        nydirty(gridding ? dirty_out.shape(1) : dirty_in.shape(1)),
        epsilon(epsilon_),
        do_wgridding(do_wgridding_),
        nthreads((nthreads_==0) ? get_default_nthreads() : nthreads_),
        verbosity(verbosity_),
        negate_v(negate_v_), divide_by_n(divide_by_n_),
        sigma_min(sigma_min_), sigma_max(sigma_max_),
        lshift(center_x), mshift(negate_v ? -center_y : center_y),
        lmshift((lshift!=0) || (mshift!=0)),
        no_nshift(!allow_nshift),
        gpu(gpu_)
      {
      timers.push("Baseline construction");
      bl = Baselines(uvw, freq, negate_v);
      MR_assert(bl.Nrows()<(uint64_t(1)<<32), "too many rows in the MS");
      MR_assert(bl.Nchannels()<(uint64_t(1)<<16), "too many channels in the MS");
      timers.pop();
      // adjust for increased error when gridding in 2 or 3 dimensions
      epsilon /= do_wgridding ? 3 : 2;
      if (!gridding)
        {
        timers.push("MS zeroing");
        quickzero(ms_out, nthreads);
        timers.pop();
        }
      scanData();
      if (nvis==0)
        {
        if (gridding) mav_apply([](Timg &v){v=Timg(0);}, nthreads, dirty_out);
        return;
        }
      auto kidx = getNuNv();
      MR_assert((nu>>logsquare)<(size_t(1)<<16), "nu too large");
      MR_assert((nv>>logsquare)<(size_t(1)<<16), "nv too large");
      ofactor = min(double(nu)/nxdirty, double(nv)/nydirty);
      krn = selectKernel<Tcalc>(ofactor, epsilon, kidx);
      supp = krn->support();
      nsafe = (supp+1)/2;
      ushift = supp*(-0.5)+1+nu;
      vshift = supp*(-0.5)+1+nv;
      maxiu0 = (nu+nsafe)-supp;
      maxiv0 = (nv+nsafe)-supp;
      vlim = min(nv/2, size_t(nv*bl.Vmax()*pixsize_y+0.5*supp+1));
      uv_side_fast = true;
      size_t vlim2 = (nydirty+1)/2+(supp+1)/2;
      if (vlim2<vlim)
        {
        vlim = vlim2;
        uv_side_fast = false;
        }
      MR_assert(nu>=2*nsafe, "nu too small");
      MR_assert(nv>=2*nsafe, "nv too small");
      MR_assert((nxdirty&1)==0, "nx_dirty must be even");
      MR_assert((nydirty&1)==0, "ny_dirty must be even");
      MR_assert((nu&1)==0, "nu must be even");
      MR_assert((nv&1)==0, "nv must be even");
      MR_assert(epsilon>0, "epsilon must be positive");
      MR_assert(pixsize_x>0, "pixsize_x must be positive");
      MR_assert(pixsize_y>0, "pixsize_y must be positive");
      countRanges();
      report();
      if (gpu)
        gridding ? x2dirty() : dirty2x_gpu();
      else
        gridding ? x2dirty() : dirty2x();

      if (verbosity>0)
        timers.report(cout);
      }
  };

template<typename Tcalc, typename Tacc, typename Tms, typename Timg> void ms2dirty(const cmav<double,2> &uvw,
  const cmav<double,1> &freq, const cmav<complex<Tms>,2> &ms,
  const cmav<Tms,2> &wgt_, const cmav<uint8_t,2> &mask_, double pixsize_x, double pixsize_y, double epsilon,
  bool do_wgridding, size_t nthreads, vmav<Timg,2> &dirty, size_t verbosity,
  bool negate_v=false, bool divide_by_n=true, double sigma_min=1.1,
  double sigma_max=2.6, double center_x=0, double center_y=0, bool allow_nshift=true, bool gpu=false)
  {
  auto ms_out(vmav<complex<Tms>,2>::build_empty());
  auto dirty_in(vmav<Timg,2>::build_empty());
  auto wgt(wgt_.size()!=0 ? wgt_ : wgt_.build_uniform(ms.shape(), 1.));
  auto mask(mask_.size()!=0 ? mask_ : mask_.build_uniform(ms.shape(), 1));
  Params<Tcalc, Tacc, Tms, Timg> par(uvw, freq, ms, ms_out, dirty_in, dirty, wgt, mask, pixsize_x, 
    pixsize_y, epsilon, do_wgridding, nthreads, verbosity, negate_v,
    divide_by_n, sigma_min, sigma_max, center_x, center_y, allow_nshift, gpu);
  }

template<typename Tcalc, typename Tacc, typename Tms, typename Timg> void dirty2ms(const cmav<double,2> &uvw,
  const cmav<double,1> &freq, const cmav<Timg,2> &dirty,
  const cmav<Tms,2> &wgt_, const cmav<uint8_t,2> &mask_, double pixsize_x, double pixsize_y,
  double epsilon, bool do_wgridding, size_t nthreads, vmav<complex<Tms>,2> &ms,
  size_t verbosity, bool negate_v=false, bool divide_by_n=true,
  double sigma_min=1.1, double sigma_max=2.6, double center_x=0, double center_y=0, bool allow_nshift=true,
  bool gpu=false)
  {
  if (ms.size()==0) return;  // nothing to do
  auto ms_in(ms.build_uniform(ms.shape(),1.));
  auto dirty_out(vmav<Timg,2>::build_empty());
  auto wgt(wgt_.size()!=0 ? wgt_ : wgt_.build_uniform(ms.shape(), 1.));
  auto mask(mask_.size()!=0 ? mask_ : mask_.build_uniform(ms.shape(), 1));
  Params<Tcalc, Tacc, Tms, Timg> par(uvw, freq, ms_in, ms, dirty, dirty_out, wgt, mask, pixsize_x,
    pixsize_y, epsilon, do_wgridding, nthreads, verbosity, negate_v,
    divide_by_n, sigma_min, sigma_max, center_x, center_y, allow_nshift, gpu);
  }

} // namespace detail_gridder

// public names
using detail_gridder::ms2dirty;
using detail_gridder::dirty2ms;

} // namespace ducc0

#endif
