import sys, os

energies = [0.0] * 2
with open(sys.argv[1], 'r') as f:
    for l in f.readlines():
        if l.startswith('DMRG Energy for root'):
            idx = int(l.split()[-3])
            energies[idx] = float(l.split()[-1])

assert abs(energies[0] - -107.654122447525) < 1E-5
assert abs(energies[1] - -106.959626154680) < 1E-5

if os.name == "nt":
    quit() # pyscf not available on windows

from pyblock2.driver.core import DMRGDriver, SymmetryTypes
from pyscf import fci
from pyscf.tools import fcidump
import numpy as np

driver = DMRGDriver(scratch="./nodex", symm_type=SymmetryTypes.SU2, n_threads=4)
driver.read_fcidump(filename='../../data/N2.STO3G.FCIDUMP', pg='d2h', iprint=0)
driver.initialize_system(n_sites=driver.n_sites, n_elec=driver.n_elec,
    spin=driver.spin, orb_sym=driver.orb_sym)
h1e, g2e, ecore = driver.h1e, driver.g2e, driver.ecore

mf = fcidump.to_scf('../../data/N2.STO3G.FCIDUMP', molpro_orbsym=True, verbose=0)
mf.mol.symmetry = True

mx = fci.addons.fix_spin_(fci.FCI(mf), ss=0)
mx.kernel(h1e, g2e, driver.n_sites, driver.n_elec, tol=1e-12,
    wfnsym=0, orbsym=np.array(driver.orb_sym), nroots=4)
assert abs(mx.e_tot[0] + ecore - -107.654122447525) < 1E-6
assert abs(mx.e_tot[1] + ecore - -106.959626154680) < 1E-6

dm1, dm2 = mx.trans_rdm12(mx.ci[0], mx.ci[1], mf.mol.nao, mf.mol.nelec)

xdm1 = np.load('./node0/1pdm-%d-%d.npy' % (0, 1))
xdm1 = xdm1[0] + xdm1[1]

assert min(np.linalg.norm(dm1 - f * xdm1) for f in [1, -1]) < 1E-5

xdm2 = np.load('./node0/2pdm-%d-%d.npy' % (0, 1))
xdm2 = xdm2[0] + xdm2[2] + xdm2[1] + xdm2[1].transpose(1, 0, 3, 2)

assert min(np.linalg.norm(dm2 - f * xdm2.transpose(0, 3, 1, 2)) for f in [1, -1]) < 1E-4
