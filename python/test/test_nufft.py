# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# Copyright(C) 2020-2022 Max-Planck-Society

from itertools import product

import ducc0
import ducc0

try:
    import finufft
    have_finufft = True
except ImportError:
    have_finufft = False
import numpy as np
import pytest
from numpy.testing import assert_allclose

pmp = pytest.mark.parametrize


def explicit_nufft(uvw, ms, shape, forward):
    xyz = np.meshgrid(*[(-(ss//2) + np.arange(ss)) for ss in shape],
                       indexing='ij')
    isign = -1 if forward else 1
    res = np.zeros(shape, dtype=ms.dtype)
    for row in range(ms.shape[0]):
       phase = sum([a*b for a,b in zip(xyz, uvw[row,:])])
       res += (ms[row]*np.exp((isign*1j)*phase))
    return res


@pmp('nx', [20, 21, 250, 257])
@pmp("npoints", (1, 37, 10))
@pmp("epsilon", (1e-1, 1e-3, 3e-5, 2e-13))
@pmp("forward", (True, False))
@pmp("singleprec", (True, False))
@pmp("nthreads", (1, 2, 7))
def test_nufft_1d(nx, npoints, epsilon, forward, singleprec, nthreads):
    if singleprec and epsilon < 1e-6:
        pytest.skip()
    rng = np.random.default_rng(42)
    uvw = (rng.random((npoints,1))-0.5)*2*np.pi
    ms = rng.random(npoints)-0.5 + 1j*(rng.random(npoints)-0.5)
    dirty = rng.random((nx))-0.5
    dirty = dirty +  1j*(rng.random((nx))-0.5)
    nu = 0
    if singleprec:
        ms = ms.astype("c8")
        dirty = dirty.astype("c8")

    def check(d2, m2):
        ref = max(ducc0.misc.vdot(ms, ms).real, ducc0.misc.vdot(m2, m2).real,
                  ducc0.misc.vdot(dirty, dirty).real, ducc0.misc.vdot(d2, d2).real)
        tol = 3e-5*ref if singleprec else 2e-13*ref
        assert_allclose(ducc0.misc.vdot(ms, m2), ducc0.misc.vdot(d2, dirty), rtol=tol)

    dirty2 = np.empty((nx,), dtype=dirty.dtype)
    dirty2 = ducc0.nufft.nu2u(points=ms, coord=uvw, forward=forward, epsilon=epsilon, nthreads=nthreads, out=dirty2).astype("c16")
    dirty_ref = explicit_nufft(uvw, ms, (nx,), forward)
    assert_allclose(ducc0.misc.l2error(dirty2,dirty_ref), 0, atol=epsilon)
    ms2 = ducc0.nufft.u2nu(grid=dirty, coord=uvw, forward=not forward, epsilon=epsilon, nthreads=nthreads).astype("c16")
    check(dirty2, ms2)

    if have_finufft and not singleprec:
        comp = finufft.nufft1d2(uvw[:,0], dirty, nthreads=nthreads,eps=epsilon,isign=1 if forward else -1)
        if comp.ndim==0:
            comp=np.array([comp[()]])
        assert_allclose(ducc0.misc.l2error(ms2,comp), 0, atol=10*epsilon)

@pmp('nx', [20, 21, 250, 257])
@pmp('ny', [21, 32, 250, 257])
@pmp("npoints", (1, 37, 10))
@pmp("epsilon", (1e-1, 1e-3, 3e-5, 2e-13))
@pmp("forward", (True, False))
@pmp("singleprec", (True, False))
@pmp("nthreads", (1, 2, 7))
def test_nufft_2d(nx, ny, npoints, epsilon, forward, singleprec, nthreads):
    if singleprec and epsilon < 1e-6:
        pytest.skip()
    rng = np.random.default_rng(42)
    uvw = (rng.random((npoints, 2))-0.5)*2*np.pi
    ms = rng.random(npoints)-0.5 + 1j*(rng.random(npoints)-0.5)
    dirty = rng.random((nx, ny))-0.5
    dirty = dirty +  1j*(rng.random((nx, ny))-0.5)
    nu = nv = 0
    if singleprec:
        ms = ms.astype("c8")
        dirty = dirty.astype("c8")

    def check(d2, m2):
        ref = max(ducc0.misc.vdot(ms, ms).real, ducc0.misc.vdot(m2, m2).real,
                  ducc0.misc.vdot(dirty, dirty).real, ducc0.misc.vdot(d2, d2).real)
        tol = 3e-5*ref if singleprec else 2e-13*ref
        assert_allclose(ducc0.misc.vdot(ms, m2), ducc0.misc.vdot(d2, dirty), rtol=tol)

    dirty2 = np.empty((nx,ny), dtype=dirty.dtype)
    dirty2 = ducc0.nufft.nu2u(points=ms, coord=uvw, forward=forward, epsilon=epsilon, nthreads=nthreads, out=dirty2).astype("c16")
    dirty_ref = explicit_nufft(uvw, ms, (nx,ny), forward)
    assert_allclose(ducc0.misc.l2error(dirty2,dirty_ref), 0, atol=epsilon)
    ms2 = ducc0.nufft.u2nu(grid=dirty, coord=uvw, forward=not forward, epsilon=epsilon, nthreads=nthreads).astype("c16")
    check(dirty2, ms2)

    if have_finufft and not singleprec:
        comp = finufft.nufft2d2(uvw[:,0], uvw[:,1], dirty, nthreads=nthreads,eps=epsilon,isign=1 if forward else -1)
        if comp.ndim==0:
            comp=np.array([comp[()]])
        assert_allclose(ducc0.misc.l2error(ms2,comp), 0, atol=10*epsilon)

@pmp('nx', [20, 21, 64])
@pmp('ny', [21, 32, 64])
@pmp('nz', [22, 35, 64])
@pmp("npoints", (1, 37, 10))
@pmp("epsilon", (1e-5, 1e-3, 3e-5, 5e-13))
@pmp("forward", (True, False))
@pmp("singleprec", (True, False))
@pmp("nthreads", (1, 2, 7))
def test_nufft_3d(nx, ny, nz, npoints, epsilon, forward, singleprec, nthreads):
    if singleprec and epsilon < 1e-6:
        pytest.skip()
    rng = np.random.default_rng(42)
    uvw = (rng.random((npoints, 3))-0.5)*2*np.pi
    ms = rng.random(npoints)-0.5 + 1j*(rng.random(npoints)-0.5)
    dirty = rng.random((nx, ny, nz))-0.5
    dirty = dirty +  1j*(rng.random((nx, ny, nz))-0.5)
    nu = nv = nw = 0
    if singleprec:
        ms = ms.astype("c8")
        dirty = dirty.astype("c8")

    def check(d2, m2):
        ref = max(ducc0.misc.vdot(ms, ms).real, ducc0.misc.vdot(m2, m2).real,
                  ducc0.misc.vdot(dirty, dirty).real, ducc0.misc.vdot(d2, d2).real)
        tol = 3e-5*ref if singleprec else 2e-13*ref
        assert_allclose(ducc0.misc.vdot(ms, m2), ducc0.misc.vdot(d2, dirty), rtol=tol)

    dirty2 = np.empty((nx,ny,nz), dtype=dirty.dtype)
    dirty2 = ducc0.nufft.nu2u(points=ms, coord=uvw, forward=forward, epsilon=epsilon, nthreads=nthreads, out=dirty2, verbosity=0).astype("c16")
    dirty_ref = explicit_nufft(uvw, ms, (nx,ny,nz), forward)
    assert_allclose(ducc0.misc.l2error(dirty2,dirty_ref), 0, atol=epsilon)
    ms2 = ducc0.nufft.u2nu(grid=dirty, coord=uvw, forward=not forward, epsilon=epsilon, nthreads=nthreads, verbosity=0).astype("c16")
    check(dirty2, ms2)

    # if have_finufft and not singleprec:
        # comp = finufft.nufft3d2(uvw[:,0], uvw[:,1], uvw[:,2], dirty, nthreads=nthreads,eps=epsilon,isign=1 if forward else -1, debug=1)
        # if comp.ndim==0:
            # comp=np.array([comp[()]])
        # assert_allclose(ducc0.misc.l2error(ms2,comp), 0, atol=30*epsilon)