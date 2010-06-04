//
// This file is part of openBliSSART.
//
// Copyright (c) 2007-2010, Alexander Lehmann <lehmanna@in.tum.de>
//                          Felix Weninger <felix@weninger.de>
//                          Bjoern Schuller <schuller@tum.de>
//
// Institute for Human-Machine Communication
// Technische Universitaet Muenchen (TUM), D-80333 Munich, Germany
//
// openBliSSART is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 2 of the License, or (at your option) any later
// version.
//
// openBliSSART is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// openBliSSART.  If not, see <http://www.gnu.org/licenses/>.
//


#include <blissart/nmf/Deconvolver.h>
#include <blissart/nmf/randomGenerator.h>
#include <blissart/linalg/generators/generators.h>
#include <blissart/ProgressObserver.h>
#include <blissart/linalg/RowVector.h>

#include <stdexcept>
#include <sstream>
#include <cmath>
#include <vector>


using namespace blissart::linalg;


namespace blissart {


namespace nmf {


// Trivial constant to avoid division by zero in multiplicative updates.
#define DIVISOR_FLOOR 1e-9


const char* Deconvolver::costFunctionName(Deconvolver::NMFCostFunction cf)
{
    if (cf == Deconvolver::EuclideanDistance) 
        return "Squared Euclidean distance";
    if (cf == Deconvolver::KLDivergence)
        return "Extended KL divergence";
    if (cf == Deconvolver::EuclideanDistanceSparse)
        return "Squared Euclidean distance + sparseness constraint";
    if (cf == Deconvolver::KLDivergenceSparse)
        return "Extended KL divergence + sparseness constraint";
    if (cf == Deconvolver::EuclideanDistanceSparseNormalized)
        return "Squared ED (normalized basis) + sparseness";
    if (cf == Deconvolver::KLDivergenceContinuous)
        return "Extended KL divergence + continuity constraint";
    // should not occur ...
    return "Unknown";
}


Deconvolver::Deconvolver(const Matrix &v, unsigned int r, unsigned int t,
                         Matrix::GeneratorFunction wGenerator,
                         Matrix::GeneratorFunction hGenerator) :
    _v(v),
    _approx(v.rows(), v.cols(), generators::zero),
    _oldApprox(0),
    _w(new Matrix*[t]),
    _wConstant(false),
    _wColConstant(new bool[r]),
    _normalizeMatrices(false),
    _t(t),
    _h(r, v.cols(), hGenerator),
    _s(r, v.cols(), generators::zero),      // zero --> no sparsity
    _c(r, v.cols(), generators::zero),      // zero --> no continuity
    _numSteps(0),
    _absoluteError(-1),
    _relativeError(-1),
    _vFrob(_v.frobeniusNorm()),
    _notificationDelay(25)
{
    if (t > v.cols()) {
        std::ostringstream errStr;
        errStr << "Invalid number of spectra: " << t
               << ": Matrix has only " << v.cols() << " columns!";
        throw std::runtime_error(errStr.str());
    }
    for (unsigned int l = 0; l < _t; ++l) {
        _w[l] = new Matrix(v.rows(), r);
    }
    for (unsigned int c = 0; c < r; ++c) {
        _wColConstant[c] = false;
    }
    generateW(wGenerator);
}


Deconvolver::~Deconvolver()
{
    for (unsigned int l = 0; l < _t; ++l) {
        delete _w[l];
    }
    delete[] _w;
    delete[] _wColConstant;
    if (_oldApprox) {
        delete _oldApprox;
    }
}


void Deconvolver::generateW(Matrix::GeneratorFunction generator)
{
    for (unsigned int l = 0; l < _t; l++) {
        for (unsigned int i = 0; i < _w[l]->rows(); i++) {
            for (unsigned int j = 0; j < _w[l]->cols(); j++) {
                _w[l]->at(i,j) = generator(i, j);
            }
        }
    }
}


void Deconvolver::generateH(Matrix::GeneratorFunction generator)
{
    for (unsigned int i = 0; i < _h.rows(); i++) {
        for (unsigned int j = 0; j < _h.cols(); j++) {
            _h(i,j) = generator(i, j);
        }
    }
}


void Deconvolver::setW(unsigned int i, const Matrix& w)
{
    assert(w.cols() == _w[i]->cols() && w.rows() == _w[i]->rows());
    delete _w[i];
    _w[i] = new Matrix(w);
}


void Deconvolver::setH(const Matrix& h)
{
    assert(h.cols() == _h.cols() && h.rows() == _h.rows());
    _h = h;
}


void Deconvolver::decompose(Deconvolver::NMFCostFunction cf,
                            unsigned int maxSteps, double eps,
                            ProgressObserver *observer)
{
    // Select an optimal algorithm according to the given parameters.
    if (cf == EuclideanDistance) {
        if (_t == 1) {
            factorizeNMFED(maxSteps, eps, observer);
        }
        else {
            factorizeNMDED(maxSteps, eps, observer);
        }
    }
    else if (cf == KLDivergence) {
        factorizeNMDKL(maxSteps, eps, observer);
    }
    else if (cf == EuclideanDistanceSparse) {
        if (_t > 1) {
            throw std::runtime_error("Sparse NMD not implemented");
        }
        factorizeNMFEDSparse(maxSteps, eps, observer);
    }
    else if (cf == KLDivergenceSparse) {
        if (_t > 1) {
            throw std::runtime_error("Sparse NMD not implemented");
        }
        factorizeNMFKLSparse(maxSteps, eps, observer);
    }
    else if (cf == KLDivergenceContinuous) {
        if (_t > 1) {
            throw std::runtime_error("Continuous NMD not implemented");
        }
        factorizeNMFKLTempCont(maxSteps, eps, observer);
    }
    else if (cf == EuclideanDistanceSparseNormalized) {
        if (_t > 1) {
            throw std::runtime_error("Sparse NMD not implemented");
        }
        factorizeNMFEDSparseNorm(maxSteps, eps, observer);
    }
    else {
        throw std::runtime_error("Invalid cost function");
    }

    // Perform post-processing if desired.
    if (_normalizeMatrices)
        normalizeMatrices();

    // Ensure the ProgressObserver sees that we have finished.
    if (observer)
        observer->progressChanged(1.0f);

    // Delete value of old approximation (used in convergence check), 
    // if it exists.
    if (_oldApprox) {
        delete _oldApprox;
        _oldApprox = 0;
    }
}


void Deconvolver::factorizeNMDKL(unsigned int maxSteps, double eps,
                                 ProgressObserver *observer)
{
    Matrix vOverApprox(_v.rows(), _v.cols());
    Matrix hShifted(_h.rows(), _h.cols());
    Matrix wUpdateNum(_v.rows(), _h.rows());
    Matrix hUpdate(_h.rows(), _h.cols());
    Matrix hUpdateMatrixNum(_h.rows(), _h.cols());
    double *wpColSums = new double[_h.rows()];

    _numSteps = 0;
    while (_numSteps < maxSteps) {

        // Compute approximation at the beginning and after the H update
        computeApprox();

        // Check convergence criterion.
        if (checkConvergence(eps, false))
            break;
        
        // Compute V/Approx.
        _v.elementWiseDivision(_approx, &vOverApprox);

        if (!_wConstant) {
            Matrix* wpH = 0;
            // Update all W_t
            hShifted = _h;
            for (unsigned int p = 0; p < _t; ++p) {
                if (_t > 1) {
                    wpH = new Matrix(_v.rows(), _v.cols());
                    // Difference-based calculation of new approximation
                    computeWpH(p, *wpH);
                    _approx.sub(*wpH);
                }
                vOverApprox.multWithTransposedMatrix(hShifted, &wUpdateNum);
                for (unsigned int j = 0; j < _w[p]->cols(); ++j) {
                    if (!_wColConstant[j]) {
                        // Precalculation of sum of row j of H
                        double hRowSum = hShifted.rowSum(j);
                        for (unsigned int i = 0; i < _w[p]->rows(); ++i) {
                            _w[p]->at(i, j) *= (wUpdateNum(i, j) / hRowSum);
                        }
                    }
                }
                if (_t > 1) {
                    computeWpH(p, *wpH);
                    _approx.add(*wpH);
                    delete wpH;
                    ensureNonnegativity(_approx);
                    hShifted.shiftColumnsRight();
                }
            }
        }

        // The standard method of computing the approximation is more efficient 
        // for T = 1 (1 vs. 2 matrix multiplications).
        if (_t == 1) {
            computeApprox();
        }

        // Now approximation has been updated in any case, 
        // so update V/approx now.
        _v.elementWiseDivision(_approx, &vOverApprox);

        // Calculate update matrix for H by averaging the updates corresponding
        // to each W_t
        hUpdate.zero();
        for (unsigned int p = 0; p < _t; ++p) {
            // Precalculation of column-sums of W_t
            for (unsigned int i = 0; i < _h.rows(); ++i) {
                wpColSums[i] = _w[p]->colSum(i);
                if (wpColSums[i] == 0.0)
                    wpColSums[i] = DIVISOR_FLOOR;
            }

            _w[p]->multWithMatrix(vOverApprox, &hUpdateMatrixNum,
                // transpose W[p]
                true, false, 
                // target dimension: R x (N-p)
                _w[p]->cols(), _w[p]->rows(), _v.cols() - p,
                0, 0, 0, p, 0, 0);
            // Calculate sum of updates
            for (unsigned int j = 0; j < _h.cols() - p; ++j) {
                for (unsigned int i = 0; i < _h.rows(); ++i) {
                    // Instead of considering the jth column of V/approx
                    // shifted p spots to the left, we consider the (j + p)th
                    // column of V/Approx itself.
                    hUpdate(i, j) += (hUpdateMatrixNum(i, j) / wpColSums[i]);
                }
            }
        }

        // Apply average update to H
        for (unsigned int j = 0; j < _h.cols(); ++j) {
            for (unsigned int i = 0; i < _h.rows(); ++i) {
                _h(i, j) *= hUpdate(i, j) / (double) _t;
            }
        }

        nextItStep(observer, maxSteps);
    }

    delete[] wpColSums;
}


void Deconvolver::factorizeNMFEDWUpdate(Matrix& w) const
{
    static Matrix wUpdateMatrixNum(_v.rows(), _h.rows());
    static Matrix wUpdateMatrixDenom(_v.rows(), _h.rows());
    static Matrix hhT(_h.rows(), _h.rows());
    double denom;
    if (!_wConstant) {
        // The trick is not to calculate (W*H)*H^T, but
        // W*(H*H^T), which is much faster, assuming common
        // dimensions of W and H.
        _v.multWithTransposedMatrix(_h, &wUpdateMatrixNum);
        _h.multWithTransposedMatrix(_h, &hhT);
        w.multWithMatrix(hhT, &wUpdateMatrixDenom);
        for (unsigned int j = 0; j < w.cols(); ++j) {
            if (!_wColConstant[j]) {
                for (unsigned int i = 0; i < w.rows(); ++i) {
                    denom = wUpdateMatrixDenom(i, j);
                    if (denom <= 0.0) denom = DIVISOR_FLOOR;
                    w(i, j) *= (wUpdateMatrixNum(i, j) / denom);
                }
            }
        }
    }
}


void Deconvolver::calculateNMFEDHUpdate(blissart::linalg::Matrix& num,
                                        blissart::linalg::Matrix& denom) const
{
    static Matrix wTw(_h.rows(), _h.rows());
    // Calculate W^T * V
    _w[0]->multWithMatrix(_v, &num, true, false,
           _h.rows(), _v.rows(), _h.cols(),
           0, 0, 0, 0, 0, 0);

    // Here the trick is to calculate (W^T * W) * H instead of
    // W^T * (W * H).
    // Calculate W^T * W
    _w[0]->multWithMatrix(*(_w[0]), &wTw, true, false,
           _h.rows(), _w[0]->rows(), _h.rows(),
           0, 0, 0, 0, 0, 0);
    wTw.multWithMatrix(_h, &denom);
}


void Deconvolver::factorizeNMFED(unsigned int maxSteps, double eps,
                                 ProgressObserver *observer)
{
    assert(_t == 1);

    Matrix& w = *(_w[0]); // for convenience
    Matrix hUpdateMatrixNum(_h.rows(), _h.cols());
    Matrix hUpdateMatrixDenom(_h.rows(), _h.cols());
    double denom;

    _numSteps = 0;
    while (_numSteps < maxSteps && !checkConvergence(eps, true)) {

        // W Update
        factorizeNMFEDWUpdate(w);

        // H Update matrices
        calculateNMFEDHUpdate(hUpdateMatrixNum, hUpdateMatrixDenom);
        for (unsigned int j = 0; j < _h.cols(); ++j) {
            for (unsigned int i = 0; i < _h.rows(); ++i) {
                denom = hUpdateMatrixDenom(i, j);
                if (denom <= 0.0) denom = DIVISOR_FLOOR;
                _h(i, j) *= (hUpdateMatrixNum(i, j) / denom);
            }
        }

        nextItStep(observer, maxSteps);
    }
}


void Deconvolver::factorizeNMDED(unsigned int maxSteps, double eps,
                                 ProgressObserver *observer)
{
    Matrix hSum(_h.rows(), _h.cols());
    Matrix wUpdateMatrixNum(_v.rows(), _h.rows());
    Matrix wUpdateMatrixDenom(_v.rows(), _h.rows());
    Matrix hUpdateMatrixNum(_h.rows(), _h.cols());
    Matrix hUpdateMatrixDenom(_h.rows(), _h.cols());
    double denom;

    _numSteps = 0;
    while (_numSteps < maxSteps) {

        // Compute approximation at the beginning and after the H update
        computeApprox();
        
        // Check convergence criterion.
        if (checkConvergence(eps, false))
            break;

        if (!_wConstant) {
            Matrix* wpH = 0;
            // Update all W[p]
            for (unsigned int p = 0; p < _t; ++p) {

                // Calculate V * (H shifted t spots to the right)^T 
                // (nominator of the update matrix)
                // In this case, zeros would be introduced in the first t rows
                // of the second factor. We can simulate this by considering
                // only the V columns starting from p.                
                _v.multWithMatrix(_h, &wUpdateMatrixNum,
                    // transpose H
                    false, true, 
                    // target dimension: MxR
                    _v.rows(), _v.cols() - p, _h.rows(),
                    0, p, 0, 0, 0, 0);

                // Calculate Approx * (H shifted t spots to the right)^T 
                // (denominator of the update matrix)
                // The same as above.
                _approx.multWithMatrix(_h, &wUpdateMatrixDenom,
                    false, true,
                    _v.rows(), _v.cols() - p, _h.rows(), 
                    0, p, 0, 0, 0, 0);

                // Efficient (difference-based) calculation of updated Approx
                // (step 1: subtraction of old W[p]*H)
                // Due to Wang (2009)
                wpH = new Matrix(_v.rows(), _v.cols());
                computeWpH(p, *wpH);

                // It is safe to overwrite Approx, as it is not directly used 
                // in the update loop.
                _approx.sub(*wpH);

                // Finally, the update loop is simple now.
                for (unsigned int j = 0; j < _w[p]->cols(); ++j) {
                    if (!_wColConstant[j]) {
                        for (unsigned int i = 0; i < _w[p]->rows(); ++i) {
                            denom = wUpdateMatrixDenom(i, j);
                            if (denom <= 0.0) denom = DIVISOR_FLOOR;
                            _w[p]->at(i, j) *= wUpdateMatrixNum(i, j) / denom;
                        }
                    }
                }

                // Calculate updated approximation, step 2
                // (addition of new W[p]*H)
                computeWpH(p, *wpH);
                _approx.add(*wpH);
                delete wpH;
                ensureNonnegativity(_approx);
            }
        }

        // Calculate update matrix for H by averaging the updates corresponding
        // to each W[p]
        hSum.zero();
        for (unsigned int p = 0; p < _t; ++p) {
            // Simulate multiplication of W[p] with V (shifted p spots to the
            // left) by considering only the columns of V starting from p;
            // We do not fill with zeros here, because we ignore the rightmost
            // p columns of the nominator and denominator matrices in
            // the update loop below.
            _w[p]->multWithMatrix(_v, &hUpdateMatrixNum,
                // transpose W[p]
                true, false, 
                // target dimension: R x (N-p)
                _w[p]->cols(), _w[p]->rows(), _v.cols() - p,
                0, 0, 0, p, 0, 0);
            _w[p]->multWithMatrix(_approx, &hUpdateMatrixDenom,
                true, false, 
                _w[p]->cols(), _w[p]->rows(), _v.cols() - p,
                0, 0, 0, p, 0, 0);

            for (unsigned int j = 0; j < _h.cols() - p; ++j) {
                for (unsigned int i = 0; i < _h.rows(); ++i) {
                    denom = hUpdateMatrixDenom(i, j);
                    // Avoid division by zero
                    if (denom <= 0.0) denom = DIVISOR_FLOOR;
                    hSum(i, j) += _h(i, j) * hUpdateMatrixNum(i, j) / denom;
                }
            }
        }

        // Apply average update to H
        for (unsigned int j = 0; j < _h.cols(); ++j) {
            for (unsigned int i = 0; i < _h.rows(); ++i) {
                _h(i, j) = hSum(i, j) / (double) _t;
            }
        }

        nextItStep(observer, maxSteps);
    }
}


void Deconvolver::factorizeNMFEDSparse(unsigned int maxSteps, double eps,
                                       ProgressObserver *observer)
{
    assert(_t == 1);

    Matrix& w = *(_w[0]);
    Matrix hUpdateMatrixNum(_h.rows(), _h.cols());
    Matrix hUpdateMatrixDenom(_h.rows(), _h.cols());
    
    // helper variables
    double denom;
    double hRowSumSq, hRowLength;

    // parts of gradient which are equal for each row
    double *csplus = new double[_h.rows()];
    double *csminus = new double[_h.rows()];

    // Precompute 2 constants
    double sqrtT = sqrt((double) _h.cols());
    double sqrtOneOverT = sqrt(1.0 / (double) _h.cols());

    _numSteps = 0;
    while (_numSteps < maxSteps && !checkConvergence(eps, true)) {

        // W Update
        factorizeNMFEDWUpdate(w);

        // H Update matrices
        calculateNMFEDHUpdate(hUpdateMatrixNum, hUpdateMatrixDenom);

        // Precompute row norms of H for normalization of sparsity weight
        // (sum of squares)
        for (unsigned int i = 0; i < _h.rows(); ++i) {
            hRowSumSq = Matrix::dotRowRow(_h, i, _h, i);
            hRowLength = sqrt(hRowSumSq);
            csplus[i] = sqrtT / hRowLength;
            csminus[i] = sqrtT * _h.rowSum(i) / (hRowSumSq * hRowLength);
        }

        for (unsigned int j = 0; j < _h.cols(); ++j) {
            for (unsigned int i = 0; i < _h.rows(); ++i) {
                denom = hUpdateMatrixDenom(i, j) + _s(i, j) * csplus[i];
                if (denom <= 0.0) denom = DIVISOR_FLOOR;
                _h(i, j) *= 
                    (hUpdateMatrixNum(i, j) + _s(i, j) * _h(i, j) * csminus[i])
                    / denom;
            }
        }

        nextItStep(observer, maxSteps);
    }

    delete[] csminus;
    delete[] csplus;
}


void Deconvolver::factorizeNMFKLSparse(unsigned int maxSteps, double eps,
                                       ProgressObserver *observer)
{
    assert(_t == 1);

    Matrix& w = *(_w[0]);
    Matrix vOverApprox(_v.rows(), _v.cols());
    Matrix wUpdateNum(_v.rows(), _h.rows());
    Matrix hUpdateMatrixNum(_h.rows(), _h.cols());
    
    // helper variables
    double denom;
    double hRowSumSq, hRowLength;

    // parts of gradient which are equal for each row
    double *csplus = new double[_h.rows()];
    double *csminus = new double[_h.rows()];

    // row sums are used for H as well as W update
    double *hRowSums = new double[_h.rows()];

    // col sums for H update
    double *wColSums = new double[w.cols()];

    // Precompute 2 constants
    double sqrtT = sqrt((double) _h.cols());
    double sqrtOneOverT = sqrt(1.0 / (double) _h.cols());

    _numSteps = 0;
    while (_numSteps < maxSteps) {

        // compute approximation
        computeApprox();

        // convergence criterion
        if (checkConvergence(eps, false))
            break;

        // numerator for W updates (fast calculation by matrix product)
        _v.elementWiseDivision(_approx, &vOverApprox);
        vOverApprox.multWithTransposedMatrix(_h, &wUpdateNum);

        // precompute H row sums
        for (unsigned int i = 0; i < _h.rows(); ++i) {
            hRowSums[i] = _h.rowSum(i);
        }

        // W Update
        if (!_wConstant) {
            for (unsigned int j = 0; j < w.cols(); ++j) {
                if (!_wColConstant[j]) {
                    for (unsigned int i = 0; i < w.rows(); ++i) {
                        w(i, j) *= (wUpdateNum(i, j) / hRowSums[j]);
                    }
                }
            }

            // recompute approximation
            computeApprox();
            _v.elementWiseDivision(_approx, &vOverApprox);
        }

        // H Update

        // Precompute row norms of H for normalization of sparsity weight
        // (sum of squares)
        for (unsigned int i = 0; i < _h.rows(); ++i) {
            hRowSumSq = Matrix::dotRowRow(_h, i, _h, i);
            hRowLength = sqrt(hRowSumSq);
            csplus[i] = sqrtT / hRowLength;
            csminus[i] = sqrtT * hRowSums[i] / (hRowSumSq * hRowLength);
            wColSums[i] = w.colSum(i);
        }

        w.multWithMatrix(vOverApprox, &hUpdateMatrixNum,
            true, false,
            _h.rows(), _v.rows(), _h.cols(),
            0, 0, 0, 0, 0, 0);
        for (unsigned int j = 0; j < _h.cols(); ++j) {
            for (unsigned int i = 0; i < _h.rows(); ++i) {
                denom = wColSums[i] + _s(i, j) * csplus[i];
                if (denom <= 0.0) denom = DIVISOR_FLOOR;
                _h(i, j) *= 
                    (hUpdateMatrixNum(i, j) + _s(i, j) * _h(i, j) * csminus[i])
                    / denom;
            }
        }

        nextItStep(observer, maxSteps);
    }

    delete[] csminus;
    delete[] csplus;
    delete[] hRowSums;
    delete[] wColSums;
}

void Deconvolver::factorizeNMFKLTempCont(unsigned int maxSteps, double eps,
                                       ProgressObserver *observer)
{
    assert(_t == 1);

    Matrix& w = *(_w[0]);
    Matrix vOverApprox(_v.rows(), _v.cols());
    Matrix wUpdateNum(_v.rows(), _h.rows());
    Matrix hUpdateMatrixNum(_h.rows(), _h.cols());
	
    // need to backup old H for proper gradient calculation
    Matrix oldH(_h.rows(), _h.cols());
    
    // helper variables
    double denom;
    double hRowSumSq, hDeltaSumSq;

    // parts of gradient which are equal for each row
    double *ctplus = new double[_h.rows()];
    double *ctminus1 = new double[_h.rows()];
    double *ctminus2 = new double[_h.rows()];

    // row sums are used for H as well as W update
    double *hRowSums = new double[_h.rows()];

    // col sums for H update
    double *wColSums = new double[w.cols()];

    _numSteps = 0;
    while (_numSteps < maxSteps) {

        // compute approximation
        computeApprox();

        // convergence criterion
        if (checkConvergence(eps, false))
            break;

        oldH = _h;

        // numerator for W updates (fast calculation by matrix product)
        _v.elementWiseDivision(_approx, &vOverApprox);
        vOverApprox.multWithTransposedMatrix(_h, &wUpdateNum);

        // precompute H row sums
        for (unsigned int i = 0; i < _h.rows(); ++i) {
            hRowSums[i] = _h.rowSum(i);
        }

        // W Update
        if (!_wConstant) {
            for (unsigned int j = 0; j < w.cols(); ++j) {
                if (!_wColConstant[j]) {
                    for (unsigned int i = 0; i < w.rows(); ++i) {
                        w(i, j) *= (wUpdateNum(i, j) / hRowSums[j]);
                    }
                }
            }

            // recompute approximation
            computeApprox();
            _v.elementWiseDivision(_approx, &vOverApprox);
        }

        // H Update

        // Row-wise precomputation of the parts of the gradient which do not 
        // depend on the column index
        for (unsigned int i = 0; i < _h.rows(); ++i) {
            hRowSumSq = Matrix::dotRowRow(_h, i, _h, i);
            
            // also precompute W column sums in this loop
            wColSums[i] = w.colSum(i);

            hDeltaSumSq = 0.0;
            for (unsigned int j = 1; j < _h.cols(); ++j) {
                double hDelta = _h(i, j) - _h(i, j-1);
                hDeltaSumSq += hDelta * hDelta;
            }

			ctplus[i] = 4 * (double) _h.cols() / hRowSumSq;
			ctminus1[i] = 2 * (double) _h.cols() / hRowSumSq;
			ctminus2[i] = 2 * (double) _h.cols() * hDeltaSumSq / (hRowSumSq * hRowSumSq);
        }

        w.multWithMatrix(vOverApprox, &hUpdateMatrixNum,
            true, false,
            _h.rows(), _v.rows(), _h.cols(),
            0, 0, 0, 0, 0, 0);

        for (unsigned int j = 0; j < _h.cols(); ++j) {
            for (unsigned int i = 0; i < _h.rows(); ++i) {
                denom = wColSums[i] + _c(i,j) * _h(i, j) * ctplus[i];
                if (denom <= 0.0) denom = DIVISOR_FLOOR;
                double l = j == 0 ? 0.0 : oldH(i, j - 1);
                double r = j == _h.cols() - 1 ? 0.0 : _h(i, j + 1);
				_h(i, j) *= 
					(hUpdateMatrixNum(i, j)  // reconstruction error
                    + _c(i,j) * ((l + r) * ctminus1[i] + _h(i,j) * ctminus2[i]))
					/ denom;
            }
        }

        nextItStep(observer, maxSteps);
    }

    delete[] ctminus1;
	delete[] ctminus2;
    delete[] ctplus;
    delete[] hRowSums;
    delete[] wColSums;
}


void Deconvolver::factorizeNMFEDSparseNorm(unsigned int maxSteps, double eps,
                                           ProgressObserver *observer)
{
    assert(_t == 1);

    Matrix& w = *(_w[0]);
    //Matrix wNormalized = w;

    Matrix wTw(_h.rows(), _h.rows());                        // r x r
    Matrix hUpdateMatrixNum(_h.rows(), _h.cols());           // r x n
    Matrix hUpdateMatrixDenom(_h.rows(), _h.cols());         // r x n

    Matrix wUpdateMatrixNum1(_v.rows(), _h.rows());          // m x r
    Matrix hhT(_h.rows(), _h.rows());                        // r x r
    Matrix wUpdateMatrixDenom1(_v.rows(), _h.rows());        // m x r

    Matrix hvT(_h.rows(), _v.rows());                        // r x m
    Matrix wUpdateMatrixNum2(_h.rows(), _h.rows());          // r x r
    Matrix wUpdateMatrixDenom2(_h.rows(), _h.rows());        // r x r
    
    double num, denom;

    _numSteps = 0;
    while (_numSteps < maxSteps && !checkConvergence(eps, true)) {
        // Normalize W
        for (unsigned int j = 0; j < w.cols(); ++j) {
            double norm = sqrt(Matrix::dotColCol(w, j, w, j));
            for (unsigned int i = 0; i < w.rows(); ++i) {
                w(i, j) = w(i, j) / norm;
            }
        }

        // H Update
        // We should keep the value of wTw here, thus we don't use 
        // calculateNMFEDHUpdate().
        // Calculate W^T * V
        w.multWithMatrix(_v, &hUpdateMatrixNum, true, false,
            _h.rows(), _v.rows(), _h.cols(),
            0, 0, 0, 0, 0, 0);
        // Calculate W^T * W
        w.multWithMatrix(w, &wTw, true, false,
            _h.rows(), w.rows(), _h.rows(),
            0, 0, 0, 0, 0, 0);
        wTw.multWithMatrix(_h, &hUpdateMatrixDenom);
        for (unsigned int j = 0; j < _h.cols(); ++j) {
            for (unsigned int i = 0; i < _h.rows(); ++i) {
                denom = hUpdateMatrixDenom(i, j) + _s(i, j);
                if (denom <= 0.0) denom = DIVISOR_FLOOR;
                _h(i, j) *= hUpdateMatrixNum(i, j) / denom;
            }
        }

        // W Update
        _v.multWithTransposedMatrix(_h, &wUpdateMatrixNum1);
        _h.multWithTransposedMatrix(_h, &hhT);
        w.multWithMatrix(hhT, &wUpdateMatrixDenom1);
        hhT.multWithMatrix(wTw, &wUpdateMatrixNum2);
        _h.multWithTransposedMatrix(_v, &hvT);
        hvT.multWithMatrix(w, &wUpdateMatrixDenom2);
        for (unsigned int j = 0; j < w.cols(); ++j) {
            if (!_wColConstant[j]) {
                for (unsigned int i = 0; i < w.rows(); ++i) {
                    num   = wUpdateMatrixNum1(i, j)
                            + wUpdateMatrixNum2(j, j) * w(i, j);
                    denom = wUpdateMatrixDenom1(i, j) 
                            + wUpdateMatrixDenom2(j, j) * w(i, j);
                    if (denom <= 0.0) denom = DIVISOR_FLOOR;
                    w(i, j) *= num / denom;
                }
            }
        }

        nextItStep(observer, maxSteps);
    }
}


bool Deconvolver::checkConvergence(double eps, bool doComputeApprox)
{
    if (eps <= 0.0)
        return false;

    bool converged;
    if (_oldApprox == 0) {
        if (doComputeApprox) {
            computeApprox();
        }
        _oldApprox = new Matrix(_approx);
        converged = false;
    }
    else {
        if (doComputeApprox) {
            computeApprox();
        }
        Matrix approxDiff(_approx);
        approxDiff.sub(*_oldApprox);
        double zeta = approxDiff.frobeniusNorm() / _oldApprox->frobeniusNorm();
        converged = zeta < eps;
        *_oldApprox = _approx;
    }
    return converged;
}


void Deconvolver::computeApprox()
{
    if (_t == 1) {
        // this is much faster
        _w[0]->multWithMatrix(_h, &_approx);
    }
    else {
        Matrix wpH(_v.rows(), _v.cols());
        _approx.zero();
        for (unsigned int p = 0; p < _t; ++p) {
            computeWpH(p, wpH);
            _approx.add(wpH);
        }
    }
}


void Deconvolver::computeWpH(unsigned int p, Matrix& wpH)
{
    // Fill W[p]*H with zeros in the first p columns
    for (unsigned int j = 0; j < p; ++j) {
        for (unsigned int i = 0; i < wpH.rows(); ++i) {
            wpH(i, j) = 0.0;
        }
    }
    // Simulate multiplication with H shifted t spots to the right:
    // only use N - p columns of H for the matrix product
    // and store the result beginning at column p of W[p]*H
    // (for this reason W[p]*H had to be filled with zeros)
    _w[p]->multWithMatrix(_h, &wpH,
        false, false,
        _w[p]->rows(), _w[p]->cols(), _h.cols() - p,
        0, 0, 0, 0, 0, p);
}


void Deconvolver::computeError()
{
    Matrix errorMatrix(_approx);
    errorMatrix.sub(_v);
    _absoluteError = errorMatrix.frobeniusNorm();
    _relativeError = _absoluteError / _vFrob;
}


void Deconvolver::ensureNonnegativity(Matrix &m, double epsilon)
{
    for (unsigned int j = 0; j < m.cols(); ++j) {
        for (unsigned int i = 0; i < m.rows(); ++i) {
            if (m(i, j) <= 0.0) {
                m(i, j) = epsilon;
            }
        }
    }
}


void Deconvolver::normalizeMatrices()
{
    // according to Wenwu Wang:
    // "We use the norm of the matrix H^q to normalise the each element in
    // H^q, i.e. each element in H^q is divided by the norm of H^q.
    // W^q(p) is normalised accordingly by multiplying each element with a norm
    // of matrix H^q(p), where H^q(p) is computed by shift H^q with p spots to
    // the right."
    double hNorm = _h.frobeniusNorm();
    for (unsigned int j = 0; j < _h.cols(); ++j) {
        for (unsigned int i = 0; i < _h.rows(); ++i) {
            _h(i, j) /= hNorm;
        }
    }

    // To simulate shift of H (as in the explanation above),
    // compute the Frobenius norms of the P-1 rightmost parts (submatrices) of H.
    // These are subtracted from the H norm.
    // Take care of p=0 by setting hNormRight[0] := 0.
    std::vector<double> hNormRight(_t, 0);
    unsigned int col = _h.cols() - 1;
    for (unsigned int p = 1; p < _t; ++p, --col) {
        if (p > 1)
            hNormRight[p - 1] = hNormRight[p - 2];
        hNormRight[p - 1] += Matrix::dotColCol(_h, col, _h, col);
    }
    for (unsigned int p = 0; p < _t; ++p) {
        for (unsigned int j = 0; j < _w[p]->cols(); ++j) {
            for (unsigned int i = 0; i < _w[p]->rows(); ++i) {
                _w[p]->at(i, j) *= (hNorm - hNormRight[p]);
            }
        }
    }
}


void Deconvolver::nextItStep(ProgressObserver *observer, 
                             unsigned int maxSteps)
{
    ++_numSteps;
    // Call the ProgressObserver every once in a while (if applicable).
    if (observer && _numSteps % _notificationDelay == 0)
        observer->progressChanged((float)_numSteps / (float)maxSteps);
}


} // namespace nmf


} // namespace blissart