#include "TNB.h"
#include "implicit.h"
#include "io.h"
#include "momentRoutines.h"
#include "units/unit_data.c"
#include <gsl/gsl_blas.h>
#include <gsl/gsl_linalg.h>
#include <gsl/gsl_vector.h>
#include <math.h>
#include <mpi.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>

static double mu;
static int Nv;
static int nspec;
static int order;
static int rank;
static double *m;
static double m_e;
static double **Q;
static double **c;
static double *M;
static int ecouple;
static int CL_type;
static int ion_type;
static int MT_or_TR;
static double collmin;
static int tauFlag;
static int TNBFlag;
static double t;

static double *n;
static double *rho;
static double **v;
static double *T;
static double *nu_tot, **nu;
static double **nu_from_MD;

// Number density under which we do not collide particles, due to lack of
// statistics
#ifndef EPS_COLL
#define EPS_COLL 1e6
#endif

// parameters for Stanton-Murillo fit data
static double a_1 = 1.4660;
static double a_2 = -1.7836;
static double a_3 = 1.4313;
static double a_4 = -0.55833;
static double a_5 = 0.061162;

static double b_0 = 0.081033;
static double b_1 = -0.091336;
static double b_2 = 0.051760;
static double b_3 = -0.50026;
static double b_4 = 0.17044;

// Assumes units:       g      1/cc       cm/s        eV
void GetMaxwell(double m, double n, double *v, double T, double *M, int sp) {
  int i, j, k, index;

  double M_prefac = n * pow(m / (2.0 * M_PI * T / ERG_TO_EV_CGS), 1.5);
  double exp_prefac = -0.5 * m * ERG_TO_EV_CGS / T;

  if (T == 0)
    for (i = 0; i < Nv * Nv * Nv; i++)
      M[i] = 0.0;
  else
#pragma omp parallel for private(i, j, k, index)
    for (i = 0; i < Nv; i++)
      for (j = 0; j < Nv; j++)
        for (k = 0; k < Nv; k++) {
          index = k + Nv * (j + Nv * i);
          M[index] = M_prefac * exp(exp_prefac * (pow(c[sp][i] - v[0], 2.0) +
                                                  pow(c[sp][j] - v[1], 2.0) +
                                                  pow(c[sp][k] - v[2], 2.0)));
        }
}

// compute 2-norm of 3d distribution function f
double norm2_f(double *f) {

  double sum = 0.0;
  int i;
  double dv = c[1] - c[0];
  double dv3 = dv * dv * dv;

  for (i = 0; i < Nv * Nv * Nv; i++)
    sum += f[i] * f[i];

  return sqrt(dv3 * sum);
}

double debyeLength_electron(double n_e, double Te) {
  double EF, lam_elec2;

  // classic debye length - electrons only
  // lam_eff = sqrt( Te /(4.0*M_PI*E_02_CGS*n_e)); //cm

  EF = 0.5 * HBAR_CGS * HBAR_CGS * pow(3.0 * M_PI * n_e, 2.0 / 3.0) /
       M_ELEC_CGS / ERG_TO_EV_CGS;
  lam_elec2 = sqrt(Te * Te + 4.0 * EF * EF / 9.0) /
              (4.0 * M_PI * E_02_CGS * n_e); // cm^2

  return lam_elec2;
}

double debyeLength_ions(double *n, double *T) {
  double TotalIonDebyeLengthInverseSquared = 0.0;
  double SpeciesIonDebyeLengthSquared;
  ;
  int sp;

  // TODO: fix for case with electrons as a species

  for (sp = 0; sp < nspec; sp++) {
    if (n[sp] != 0.0) {
      SpeciesIonDebyeLengthSquared =
          T[sp] / (4.0 * M_PI * E_02_CGS * n[sp]); // cm^2
      TotalIonDebyeLengthInverseSquared +=
          1.0 / SpeciesIonDebyeLengthSquared; // 1/cm^2;
    }
  }

  return TotalIonDebyeLengthInverseSquared;
}

double debyeLength(double *n, double *T, double n_e, double Te) {
  double ElectronDebyeLengthSquared;
  double TotalIonDebyeLengthInverseSquared;
  double EffectiveScreeningLength;

  ElectronDebyeLengthSquared = debyeLength_electron(n_e, Te);
  TotalIonDebyeLengthInverseSquared = debyeLength_ions(n, T);

  // improved debye length from Stanton-Murillo
  EffectiveScreeningLength =
      pow(1.0 / ElectronDebyeLengthSquared + TotalIonDebyeLengthInverseSquared,
          -0.5);
  return EffectiveScreeningLength;
}

// de broglie wavelength squared - cm^2
double deBroglieLength2(double Te) {

  double lambdaDeBroglie =
      pow(HBAR_CGS, 2) / (4.0 * M_ELEC_CGS * Te) / ERG_TO_EV_CGS;
  return lambdaDeBroglie;
}

// Distance of closest approach - cm^2
double closestApproach2(double Z, double T) {

  double closest = pow(Z * E_02_CGS / T, 2);

  return closest;
}

// Units - cm
double ionSphereRadius(double n) {

  double a_i = pow(3.0 / (4.0 * M_PI * n), 1.0 / 3.0);

  return a_i;
}

void getColl(double *n, double *T, double Te, double *Z, double *nuij,
             double *nuji, int i, int j) {

  double lam_eff, n_e;
  double alpha_SM, K_11, K;
  double g5_int;
  double a_i, lambda_eff, lambda_db, b90_2, lam_ratio;
  double logLam, logLam_ii, logLam_ij;
  double V02;
  double nu11, nu12, nu21;

  int sp;

  n_e = 0.0;
  for (sp = 0; sp < nspec; sp++)
    n_e += Z[sp] * n[sp];

  lam_eff = debyeLength(n, T, n_e, Te);

  // reduced mass
  if (i >= 0) {
    mu = m[i] * m[j] / (m[i] + m[j]);

    V02 = pow(Z[i] * Z[j] * E_02_CGS, 2); // eV^2 cm^2
  } else {
    mu = m_e * m[j] / (m_e + m[j]);

    V02 = pow(Z[j] * E_02_CGS, 2); // eV^2 cm^2
  }
  // printf("%d %d %d %d %d %d %g\n",i,j,ecouple,ion_type,MT_or_TR,CL_type, mu);

  // Now to start branching off for collision rates

  // First check to see if these are electron collisions
  // Note: there are really two cases here depending on ecouple
  // Case One: i = 0 and ecouple = 2
  //    This is when the electrons are species 0 and we are finding coll rates
  //    related to this
  // Case Two: ecouple = 1 and i is set to -1
  //    This is when the electrons are a fixed background temperature. i=-1 is
  //    just what we send when we just want
  //    the rates back since they are not an actual species
  if (((i == 0) && (ecouple == 2)) || (i < 0)) {

    // Find Coulomb Logarithm
    if (CL_type == 0) {            // GMS
      a_i = ionSphereRadius(n[j]); // cm

      lambda_eff = debyeLength_electron(n_e, Te);

      lambda_db = deBroglieLength2(Te);

      b90_2 = closestApproach2(Z[j], T[j]);

      logLam = 0.5 * log(1 + (lambda_eff + a_i * a_i) /
                                 (lambda_db + b90_2)); // GMS CL
      logLam_ii = logLam;
      logLam_ij = logLam;
    } else if (CL_type == 1) { // NRL

      // NRL electron-electron CL
      logLam_ii = 23.5 - log(sqrt(n_e) * pow(Te, -5.0 / 4.0)) -
                  sqrt(1e-5 + (log(Te) - 2) * (log(Te) - 2) / 16);

      // NRL electron-ion CL
      logLam_ij = 24 - log(sqrt(n_e) / Te);
    } else if (CL_type == 2) { // B-Y generated

      lambda_eff = Te / (4.0 * M_PI * E_02_CGS *
                         n_e); // electron debye length squared - cm^2
      lambda_db = pow(HBAR_CGS, 2) / (4.0 * M_ELEC_CGS * Te) /
                  ERG_TO_EV_CGS; // de broglie wavelength squared - cm^2
      lam_ratio = (lambda_eff / lambda_db);

      logLam = 0.5 * (-lam_ratio / (1 + lam_ratio) + log(1 + lam_ratio));
      logLam_ii = logLam;
      logLam_ij = logLam;
    }

    // Now get the actual collision rate
    if (ion_type == 0) { // Landau-Spitzer with whatever the CL is
      g5_int = 0.5 * M_PI * V02 * (m_e + m[j]) * (m_e + m[j]) /
               pow(m_e * T[j] + m[j] * Te, 2);

      if (MT_or_TR == 0) { // MT case

        // Coll rates based on Morse notation
        nu11 = 64.0 * M_PI * n_e * mu * sqrt(m_e * Te + m_e * Te) /
               (3.0 * pow(2.0 * M_PI, 1.5) * m_e * sqrt(m_e * m[i])) * g5_int *
               logLam_ii / sqrt(ERG_TO_EV_CGS);
        nu12 = 64.0 * M_PI * n[j] * mu * sqrt(m_e * T[j] + m[j] * Te) /
               (3.0 * pow(2.0 * M_PI, 1.5) * m_e * sqrt(m_e * m[j])) * g5_int *
               logLam_ij / sqrt(ERG_TO_EV_CGS);
        nu21 = 64.0 * M_PI * n_e * mu * sqrt(m_e * T[j] + m[j] * Te) /
               (3.0 * pow(2.0 * M_PI, 1.5) * m[j] * sqrt(m_e * m[j])) * g5_int *
               logLam_ij / sqrt(ERG_TO_EV_CGS);

      }

      else if (MT_or_TR == 1) { // Temp relax case

        // Coll rates based on Morse notation
        nu11 = 128.0 * M_PI * n_e * mu * mu * sqrt(m_e * Te + m_e * Te) /
               (3.0 * pow(2.0 * M_PI * m_e * m_e, 1.5)) * g5_int * logLam_ii /
               sqrt(ERG_TO_EV_CGS);
        nu12 = 128.0 * M_PI * n[j] * mu * mu * sqrt(m_e * T[j] + m[j] * Te) /
               (3.0 * pow(2.0 * M_PI * m_e * m[j], 1.5)) * g5_int * logLam_ij /
               sqrt(ERG_TO_EV_CGS);
        nu21 = 128.0 * M_PI * n_e * mu * mu * sqrt(m_e * T[j] + m[j] * Te) /
               (3.0 * pow(2.0 * M_PI * m_e * m[j], 1.5)) * g5_int * logLam_ij /
               sqrt(ERG_TO_EV_CGS);
      }

    } else if (ion_type == 1) { // NRL
      if (MT_or_TR == 0) {      // momentum transfer

        // e-e "slow" column in NRL
        nu11 = 2.0 * n_e * logLam_ii * 5.8e-6 / pow(Te, 1.5);

        // e-i "fast" column in NRL
        nu12 = 2.0 * 3.9e-6 * n[j] * pow(Z[j], 2) * logLam_ij / pow(Te, 1.5);

        // i-e "slow" column in NRL
        nu21 = 2.0 * 1.6e-9 * n_e * (M_P_CGS / m[j]) * pow(Z[j], 2) *
               logLam_ij / pow(Te, 1.5);
        // printf("i: %d j: %d loglam: %g\n",i,j,logLam_ij);
      } else if (MT_or_TR == 1) { // temperature relaxation

        nu11 = 2.0 * 1.8e-19 * m_e * n_e * logLam_ii /
               pow(m_e * Te + m_e * Te, 1.5);

        nu12 = 2.0 * 1.8e-19 * sqrt(m_e * m[j]) * Z[j] * Z[j] * n[j] *
               logLam_ij / pow(m_e * T[j] + m[j] * Te, 1.5);

        nu21 = 2.0 * 1.8e-19 * sqrt(m_e * m[j]) * Z[j] * Z[j] * n_e *
               logLam_ij / pow(m_e * T[j] + m[j] * Te, 1.5);
      }
    }
  } else { // ion-ion case

    if (ion_type == 0) { // Collision rates based on S+M

      alpha_SM = m[i] * m[j] * Z[i] * Z[j] * E_02_CGS /
                 (mu * lam_eff * (m[i] * T[j] + m[j] * T[i]));
      if (alpha_SM < 1.0)
        K_11 = -0.25 * log(a_1 * alpha_SM + a_2 * pow(alpha_SM, 2) +
                           a_3 * pow(alpha_SM, 3) + a_4 * pow(alpha_SM, 4) +
                           a_5 * pow(alpha_SM, 5));
      else
        K_11 = (b_0 + b_1 * log(alpha_SM) + b_2 * pow(log(alpha_SM), 2)) /
               (1 + b_3 * alpha_SM + b_4 * alpha_SM * alpha_SM);

      K = m[i] * m[j] * 0.5 / (m[i] * T[j] + m[j] * T[i]);

      g5_int = 2.0 * M_PI * pow(2.0 * K * Z[i] * Z[j] * E_02_CGS / mu, 2) *
               K_11 / 4.0;

      if (MT_or_TR == 0) { // momentum transfer
        nu11 = 64.0 * M_PI * n[i] * mu * sqrt(m[i] * T[i] + m[i] * T[i]) /
               (3.0 * sqrt(2.0 * M_PI) * m[i] * sqrt(m[i] * m[j])) * g5_int /
               sqrt(ERG_TO_EV_CGS);
        nu12 = 64.0 * M_PI * n[j] * mu * sqrt(m[i] * T[j] + m[j] * T[i]) /
               (3.0 * sqrt(2.0 * M_PI) * m[i] * sqrt(m[i] * m[j])) * g5_int /
               sqrt(ERG_TO_EV_CGS);
        nu21 = 64.0 * M_PI * n[i] * mu * sqrt(m[i] * T[j] + m[j] * T[i]) /
               (3.0 * sqrt(2.0 * M_PI) * m[j] * sqrt(m[i] * m[j])) * g5_int /
               sqrt(ERG_TO_EV_CGS);
      } else if (MT_or_TR == 1) { // energy transfer
        nu11 = 128.0 * M_PI * n[i] * mu * mu * sqrt(m[i] * T[j] + m[j] * T[i]) /
               (3.0 * sqrt(2.0 * M_PI) * pow(m[i] * m[j], 1.5)) * g5_int /
               sqrt(ERG_TO_EV_CGS);
        nu12 = 128.0 * M_PI * n[j] * mu * mu * sqrt(m[i] * T[j] + m[j] * T[i]) /
               (3.0 * sqrt(2.0 * M_PI) * pow(m[i] * m[j], 1.5)) * g5_int /
               sqrt(ERG_TO_EV_CGS);
        nu21 = 128.0 * M_PI * n[i] * mu * mu * sqrt(m[i] * T[j] + m[j] * T[i]) /
               (3.0 * sqrt(2.0 * M_PI) * pow(m[i] * m[j], 1.5)) * g5_int /
               sqrt(ERG_TO_EV_CGS);
      }
    }

    else if (ion_type == 1) { // NRL CL and mom transfer coll rates

      // Get Coulomb Log
      if (CL_type == 0) {                                // GMS
        a_i = pow(3.0 / (4.0 * M_PI * n[j]), 1.0 / 3.0); // cm

        lambda_eff = Te / (4.0 * M_PI * E_02_CGS *
                           n_e); // electron debye length squared - cm^2

        lambda_db = HBAR_CGS * HBAR_CGS / (4.0 * M_ELEC_CGS * Te) /
                    ERG_TO_EV_CGS; // de broglie wavelength squared - cm^2

        b90_2 = pow(Z[j] * E_02_CGS / T[j], 2); // closest approach - cm^2
        logLam = 0.5 * log(1 + (lambda_eff + a_i * a_i) /
                                   (lambda_db + b90_2)); // GMS CL
        logLam_ii = logLam;
        logLam_ij = logLam;
      } else if (CL_type == 1) { // NRL

        logLam_ii =
            23.0 - log((Z[i] * Z[i] / T[i]) * sqrt(n[i] * Z[i] * Z[i] / T[i] +
                                                   n[i] * Z[i] * Z[i] / T[i]));

        logLam_ij =
            23.0 -
            log(Z[i] * Z[j] * (m[i] + m[j]) / (m[i] * T[j] + m[j] * T[i]) *
                sqrt(n[i] * Z[i] * Z[i] / T[i] + n[j] * Z[j] * Z[j] / T[j]));
        // printf("%d %d %g %g\n",i,j,logLam_ii,logLam_ij);
      }

      if (MT_or_TR == 0) {
        nu11 = 6.8e-8 * sqrt(M_P_CGS) * sqrt(m[i] / (m[i] * (m[i] + m[i]))) /
               pow(T[i], 1.5) * n[i] * pow(Z[i], 4) * logLam_ii;
        nu12 = 2.0 * 6.8e-8 * sqrt(M_P_CGS) *
               sqrt(m[j] / (m[i] * (m[i] + m[j]))) / pow(T[j], 1.5) * n[j] *
               pow(Z[i], 2) * pow(Z[j], 2) * logLam_ij;
        nu21 = 2.0 * 6.8e-8 * sqrt(M_P_CGS) *
               sqrt(m[i] / (m[j] * (m[i] + m[j]))) / pow(T[i], 1.5) * n[i] *
               pow(Z[i], 2) * pow(Z[j], 2) * logLam_ij;
      } else if (MT_or_TR == 1) {

        nu11 = 1.8e-19 * m[i] * n[i] * logLam_ii /
               pow(m[j] * T[i] + m[i] * T[j], 1.5);
        nu12 = 2.0 * 1.8e-19 * sqrt(m[i] * m[j]) * Z[i] * Z[i] * Z[j] * Z[j] *
               n[j] * logLam_ij / pow(m[i] * T[j] + m[j] * T[i], 1.5);
        nu21 = 2.0 * 1.8e-19 * sqrt(m[i] * m[j]) * Z[i] * Z[i] * Z[j] * Z[j] *
               n[i] * logLam_ij / pow(m[i] * T[j] + m[j] * T[i], 1.5);
      }
    } else {
      printf("Collision type not specified\n Please put: \n 0: Stanton-Murillo "
             "rates \n 1: NRL rates\n");
      exit(37);
    }
  }

  if (i == j) {
    *nuij = nu11;
    *nuji = nu11;
  } else {
    *nuij = nu12;
    *nuji = nu21;
  }
  // printf("%d %d %g %g\n",i,j,1.0 / *nuij,1.0 / *nuji);
}

void initialize_BGK(double ns, int numV, double *mass, double **vels, int ord,
                    int ec, int CL, int itype, int MorT, int tFlag, int TNB,
                    char *filename) {
  int i;

  nspec = ns;
  m = mass;
  Nv = numV;
  c = vels;
  order = ord;

  ecouple = ec;
  CL_type = CL;
  ion_type = itype;
  MT_or_TR = MorT;
  tauFlag = tFlag;
  TNBFlag = TNB;

  collmin = 1.0;

  m_e = M_ELEC_CGS;

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // Allocate

  Q = malloc(ns * sizeof(double *));

  for (i = 0; i < ns; i++) {
    Q[i] = malloc(Nv * Nv * Nv * sizeof(double));
  }

  M = malloc(Nv * Nv * Nv * sizeof(double));

  // alloc moment vectors
  n = malloc(ns * sizeof(double));
  rho = malloc(ns * sizeof(double));
  nu_tot = malloc(ns * sizeof(double));
  v = malloc(ns * sizeof(double *));
  nu = malloc(ns * sizeof(double *));
  for (i = 0; i < ns; i++) {
    v[i] = malloc(3 * sizeof(double));
    nu[i] = malloc(ns * sizeof(double));
  }
  T = malloc(ns * sizeof(double));

  t = 0.0;

  // load taus from MD, if applicable
  if (tauFlag == 1) {
    nu_from_MD = malloc(ns * sizeof(double *));
    for (i = 0; i < ns; i++) {
      nu_from_MD[i] = malloc(ns * sizeof(double));
    }
    printf("Loading taus from %s \n", filename);
    load_taus_homog(nu_from_MD, filename);
  }
}

// Does explicit update of the distribution functions f at a single grid point.
// Moments are re-calculated.

void BGK_ex(double **f, double **f_out, double *Z, double dt, double Te) {

  double ntot, rhotot;

  // Maxwellian params
  double mixU[3], mixU_sq;

  double mixT;
  double v2_1, v2_2;

  // coll operator stuff
  double n_e;
  double nu11, nu12, nu21;

  int i, j, k;

  // get moments

  ntot = 0.0;
  rhotot = 0.0;
  for (i = 0; i < nspec; i++) {
    n[i] = getDensity(f[i], i);
    rho[i] = m[i] * n[i];
    ntot += n[i];
    rhotot += m[i] * n[i];

    getBulkVel(f[i], v[i], n[i], i);
  }

  // Find temperatures BASED ON INDIVIDUAL SPECIES VELOCITY. Note - result is in
  // eV
  for (i = 0; i < nspec; i++) {
    T[i] = getTemp(m[i], n[i], v[i], f[i], i);
  }

  // check for blowup
  if (isnan(n[0])) {
    printf("Something weird is going on: What did Jan Say? The Michael Scott "
           "Story. By Michael Scott. With Dwight Schrute.\n NaN detected in "
           "BGK.c\n");
    for (i = 0; i < nspec; i++) {
      printf("%d n: %g v: %g T: %g Z: %g Te: %g\n", i, n[i], v[i][0], T[i],
             Z[i], Te);
    }
    exit(1);
  }

  // Now generate the BGK operators

  // explicit update

  // initialize
  for (i = 0; i < nspec; i++)
    for (k = 0; k < Nv * Nv * Nv; k++)
      f_out[i][k] = 0.0;

  ////////////////////////////
  // Find the collision rates//
  ////////////////////////////

  // NOTE: this can probably be refactored/split off for clarity

  // do ij and ji at the same time
  for (i = 0; i < nspec; i++) {
    for (j = i; j < nspec; j++) {

      mu = m[i] * m[j] / (m[i] + m[j]);

      if (tauFlag == 0) {
        if ((n[i] > EPS_COLL) && (n[j] > EPS_COLL)) {
          getColl(n, T, Te, Z, &nu12, &nu21, i, j);
          collmin = (collmin < 1.0 / nu12) ? collmin : 1.0 / nu12;
          collmin = (collmin < 1.0 / nu21) ? collmin : 1.0 / nu21;

          if (i == j)
            nu11 = nu12;
        } else {
          nu11 = 0.0;
          nu12 = 0.0;
          nu21 = 0.0;
        }
      } else if (tauFlag == 1) {
        nu11 = nu_from_MD[i][i];
        nu12 = nu_from_MD[i][j];
        nu21 = nu_from_MD[j][i];
      } else {
        printf("Error: set tauflag to 0 or 1\n");
        exit(37);
      }

      // if(i != j)
      // printf("i: %d j: %d tauij: %g tauji: %g Te:
      // %g\n",i,j,1.0/nu12,1.0/nu21,Te);

      // explicit first order update

      if (i == j) {
        if (n[i] >= EPS_COLL) {

          GetMaxwell(m[i], n[i], v[i], T[i], M, i);
#pragma omp parallel for private(k)
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_out[i][k] += nu11 * (M[k] - f[i][k]);
        }
        // Check do see if this is DD, do TNB if needed
        if ((TNBFlag > 0) && (mu > 1.6e-24) && (mu < 1.7e-24)) {
          TNB_DD(f, f_out, i, rank, TNBFlag, 1.0, mu, n, v, T);
        }
      } else { // ij case
        if (!((n[i] < EPS_COLL) || (n[j] < EPS_COLL))) {

          // Get Maxwell cross terms
          mixU_sq = 0.0;
          v2_1 = 0.0;
          v2_2 = 0.0;
          for (k = 0; k < 3; k++) {
            mixU[k] = (rho[i] * nu12 * v[i][k] + rho[j] * nu21 * v[j][k]) /
                      (rho[i] * nu12 + rho[j] * nu21);
            mixU_sq += mixU[k] * mixU[k];
            v2_1 += v[i][k] * v[i][k];
            v2_2 += v[j][k] * v[j][k];
          }

          if (tauFlag == 1) {
            // original formula for mixT
            mixT = (n[i] * nu12 * T[i] + n[j] * nu21 * T[j]) /
                       (n[i] * nu12 + n[j] * nu21) +
                   ERG_TO_EV_CGS *
                       (rho[i] * nu12 * (v2_1 - mixU_sq) +
                        rho[j] * nu21 * (v2_2 - mixU_sq)) /
                       (3.0 * (n[i] * nu12 + n[j] * nu21));
          } else {
            // simplified formulas for mixT
            double vdiff2 = (v[i][0] - v[j][0]) * (v[i][0] - v[j][0]) +
                            (v[i][1] - v[j][1]) * (v[i][1] - v[j][1]) +
                            (v[i][2] - v[j][2]) * (v[i][2] - v[j][2]);

            if (MT_or_TR == 0) {
              mixT = (m[j] * T[i] + m[i] * T[j]) / (m[i] + m[j]) +
                     (m[i] * m[j]) / (6.0 * (m[i] + m[j])) * ERG_TO_EV_CGS *
                         vdiff2;
            } else {
              mixT = 0.5 * (T[i] + T[j]) + (m[i] * m[j]) /
                                               (6.0 * (m[i] + m[j])) *
                                               ERG_TO_EV_CGS * vdiff2;
            }
          }

          if (mixT < 0) {
            printf("Negative mixture temperature! Aborting.\n");
            printf("%d %d %g %g %g %g %g\n", i, j, n[i], n[j], T[i], T[j], Te);
            exit(37);
          }

          GetMaxwell(m[i], n[i], mixU, mixT, M, i);
#pragma omp parallel for private(k)
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_out[i][k] += nu12 * (M[k] - f[i][k]);

          GetMaxwell(m[j], n[j], mixU, mixT, M, j);
#pragma omp parallel for private(k)
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_out[j][k] += nu21 * (M[k] - f[j][k]);
        }

        // Check for DT reaction
        if ((TNBFlag > 0) && (mu > 1.8e-24) && (mu < 2.0e-24)) {
          TNB_DT(f, f_out, i, j, rank, TNBFlag, 1.0, mu, n, v, T);
        }
      }
    }
  }

  // now check to see if we are also colliding with untracked background
  // electrons
  if (ecouple == 1) {

    n_e = 0.0;
    for (i = 0; i < nspec; i++)
      n_e += n[i] * Z[i];

    for (j = 0; j < nspec; j++) {
      getColl(
          n, T, Te, Z, &nu12, &nu21, -1,
          j); //-1 says the other species is electrons with a fixed temperature

      collmin = (collmin < 1.0 / nu21) ? collmin : 1.0 / nu21;

      if (n[j] > EPS_COLL) {

        // Get Maxwell cross terms

        // velocity - assume electron and ion velocities are the same.
        for (k = 0; k < 3; k++)
          mixU[k] = v[j][k];

        // Temperature - electron temperature is fixed, velo terms go away.
        mixT =
            (n_e * nu12 * Te + n[j] * nu21 * T[j]) / (n_e * nu12 + n[j] * nu21);
        // printf("Te %g Tj %g mix %g nu12 %g nu21
        // %g\n",Te,T[j],mixT,1/nu12,1/nu21);

        if (mixT < 0) {
          printf("Negative mixture temperature! Aborting.\n");
          exit(37);
        }

        GetMaxwell(m[j], n[j], mixU, mixT, M, j);
#pragma omp parallel for private(k)
        for (k = 0; k < Nv * Nv * Nv; k++)
          f_out[j][k] += nu21 * (M[k] - f[j][k]);
      }
    }
  }

  // printf("collmin %g\n",collmin);
}

void BGK_Greene(double **f, double **f_out, double *Z, double dt, double beta,
                double Te) {
  double ntot, rhotot;

  // Maxwellian params
  double mixU[3], mixU_sq;
  double mixU2[3], mixU2_sq;
  double mixT, mixT2;
  double v12_2;

  // coll operator stuff
  double nu11, nu12, nu21;

  int i, j, k;

  // get moments

  ntot = 0.0;
  rhotot = 0.0;
  for (i = 0; i < nspec; i++) {
    n[i] = getDensity(f[i], i);
    rho[i] = m[i] * n[i];
    ntot += n[i];
    rhotot += m[i] * n[i];

    getBulkVel(f[i], v[i], n[i], i);
  }

  // Find temperatures BASED ON INDIVIDUAL SPECIES VELOCITY. Note - result is in
  // eV
  for (i = 0; i < nspec; i++) {
    T[i] = getTemp(m[i], n[i], v[i], f[i], i);
  }

  // Now generate the BGK operators

  // explicit update

  // initialize
  for (i = 0; i < nspec; i++)
    for (k = 0; k < Nv * Nv * Nv; k++)
      f_out[i][k] = 0.0;

  ////////////////////////////
  // Find the collision rates//
  ////////////////////////////

  // NOTE: this can probably be refactored/split off for clarity

  // do ij and ji at the same time
  for (i = 0; i < nspec; i++) {
    for (j = i; j < nspec; j++) {

      getColl(n, T, Te, Z, &nu12, &nu21, i, j);
      collmin = (collmin < 1.0 / nu12) ? collmin : 1.0 / nu12;
      collmin = (collmin < 1.0 / nu21) ? collmin : 1.0 / nu21;

      if (i == j)
        nu11 = nu12;
      // printf("%d %d %g %g\n",i,j,nu12,nu21);

      // explicit first order update

      // define the Beta term and effective coll rates
      if (i != j) {
        nu12 = nu12 / (1.0 + beta);
        nu21 = nu21 / (1.0 + beta);
        // printf("Beta: %g Cross rates: %g %g\n",beta,1.0/nu12,1.0/nu21);
      }
      // Note - the getCall gives nu_ij = (m_i + m_j) * alpha / rho_i

      if (i == j) {
        if (n[j] != 0) {

          GetMaxwell(m[i], n[i], v[i], T[i], M, i);
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_out[i][k] += nu11 * (M[k] - f[i][k]);
        }
      } else {
        if (!((n[i] == 0) || (n[j] == 0))) {

          // Get Maxwell cross terms
          mixU_sq = 0.0;
          mixU2_sq = 0.0;
          v12_2 = 0.0;
          for (k = 0; k < 3; k++) {
            mixU[k] =
                0.5 * (v[i][k] + v[j][k]) - 0.5 * beta * (v[i][k] - v[j][k]);
            mixU_sq += mixU[k] * mixU[k];
            mixU2[k] =
                0.5 * (v[i][k] + v[j][k]) + 0.5 * beta * (v[i][k] - v[j][k]);
            mixU2_sq += mixU2[k] * mixU2[k];
            v12_2 += (v[i][k] - v[j][k]) * (v[i][k] - v[j][k]);
          }

          mixT = (m[j] * T[i] + m[i] * T[j]) / (m[i] + m[j]) -
                 beta * m[i] / (m[i] + m[j]) * (T[i] - T[j]) +
                 (1.0 / 6.0) * (1.0 - beta * beta) * m[i] * m[j] /
                     (m[i] + m[j]) * v12_2 * ERG_TO_EV_CGS +
                 (1.0 / 12.0) * (1.0 + beta) * (1.0 + beta) * (m[j] - m[i]) /
                     (m[i] + m[j]) * m[i] * v12_2 * ERG_TO_EV_CGS;

          mixT2 = (m[j] * T[i] + m[i] * T[j]) / (m[i] + m[j]) +
                  beta * m[j] / (m[i] + m[j]) * (T[i] - T[j]) +
                  (1.0 / 6.0) * (1.0 - beta * beta) * m[i] * m[j] /
                      (m[i] + m[j]) * v12_2 * ERG_TO_EV_CGS -
                  (1.0 / 12.0) * (1.0 + beta) * (1.0 + beta) * (m[j] - m[i]) /
                      (m[i] + m[j]) * m[j] * v12_2 * ERG_TO_EV_CGS;

          // printf("MixT1 %g  MixT2 %g\n",mixT,mixT2);

          if ((mixT < 0) || (mixT2 < 0)) {
            printf("Negative mixture temperature! Aborting.");
            exit(37);
          }

          GetMaxwell(m[i], n[i], mixU, mixT, M, i);
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_out[i][k] += nu12 * (M[k] - f[i][k]);

          GetMaxwell(m[j], n[j], mixU2, mixT2, M, j);
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_out[j][k] += nu21 * (M[k] - f[j][k]);
        }
      }
    }
  }

  // check for blowup
  if (isnan(n[0])) {
    printf("Something weird is going on: What did Jan Say? The Michael Scott "
           "Story. By Michael Scott. With Dwight Schrute.\n NaN detected \n");
    exit(1);
  }
  // printf("collmin %g\n",collmin);
}

void BGK_NRL(double **f, double **f_out, double *Z, double dt, double Te) {
  double ntot, rhotot;

  // coll operator stuff
  double nu11, nu12, nu21;

  int i, j, k;

  // get moments

  ntot = 0.0;
  rhotot = 0.0;
  for (i = 0; i < nspec; i++) {
    n[i] = getDensity(f[i], i);
    rho[i] = m[i] * n[i];
    ntot += n[i];
    rhotot += m[i] * n[i];

    getBulkVel(f[i], v[i], n[i], i);
  }

  // Find temperatures BASED ON INDIVIDUAL SPECIES VELOCITY. Note - result is in
  // eV
  for (i = 0; i < nspec; i++) {
    T[i] = getTemp(m[i], n[i], v[i], f[i], i);
  }

  // Now generate the BGK operators

  // explicit update

  // initialize
  for (i = 0; i < nspec; i++)
    for (k = 0; k < Nv * Nv * Nv; k++)
      f_out[i][k] = 0.0;

  ////////////////////////////
  // Find the collision rates//
  ////////////////////////////

  // NOTE: this can probably be refactored/split off for clarity

  // do ij and ji at the same time
  for (i = 0; i < nspec; i++) {
    for (j = i; j < nspec; j++) {

      getColl(n, T, Te, Z, &nu12, &nu21, i, j);
      collmin = (collmin < 1.0 / nu12) ? collmin : 1.0 / nu12;
      collmin = (collmin < 1.0 / nu21) ? collmin : 1.0 / nu21;

      // if using Morse-style formulas adjust by 0.5 because of difference in
      // BGK formulas
      if (ion_type == 0) {
        nu12 = 0.5 * nu12;
        nu21 = 0.5 * nu21;
      }

      if (i == j)
        nu11 = nu12;
      // printf("%d %d %g %g\n",i,j,1.0/nu12,1.0/nu21);

      // explicit first order update

      if (i == j) {
        if (n[j] != 0) {

          GetMaxwell(m[i], n[i], v[i], T[i], M, i);
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_out[i][k] += nu11 * (M[k] - f[i][k]);
        }
      } else {
        if (!((n[i] == 0) || (n[j] == 0))) {

          GetMaxwell(m[i], n[i], v[j], T[j], M, i);
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_out[i][k] += nu12 * (M[k] - f[i][k]);

          GetMaxwell(m[j], n[j], v[i], T[i], M, j);
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_out[j][k] += nu21 * (M[k] - f[j][k]);
        }
      }
    }
  }

  // check for blowup
  if (isnan(n[0])) {
    printf("Something weird is going on: What did Jan Say? The Michael Scott "
           "Story. By Michael Scott. With Dwight Schrute.\n NaN detected \n");
    exit(1);
  }
  // printf("collmin %g\n",collmin);
}

// Does implicit update of the distribution functions f at a single grid point.
// Moments are re-calculated.
void BGK_im_linear(double **f, double **f_out, double *Z, double dt,
                   double Te) {

  int i, j;

  double ntot, rhotot;

  // coll operator stuff
  double nu12, nu21;

  // If electrons are a 'background' species, we need to include their moments
  // in the update vector
  // When we do C++ this will be much easier...

  int nspec_linear = (ecouple == 1) ? (nspec + 1) : nspec;
  double *m_linear = malloc(nspec_linear * sizeof(double));
  double *n_linear = malloc(nspec_linear * sizeof(double));
  double **v_linear = malloc(nspec_linear * sizeof(double *));
  for (i = 0; i < nspec_linear; i++) {
    v_linear[i] = malloc(3 * sizeof(double));
  }
  double *T_linear = malloc(nspec_linear * sizeof(double));

  // collision rate array
  double **nu_linear = malloc(nspec_linear * sizeof(double *));

  for (i = 0; i < nspec_linear; i++)
    nu_linear[i] = malloc(nspec_linear * sizeof(double));

  // get moments
  ntot = 0.0;
  rhotot = 0.0;
  for (i = 0; i < nspec; i++) {
    m_linear[i] = m[i];
    n_linear[i] = getDensity(f[i], i);
    rho[i] = m[i] * n_linear[i];
    ntot += n_linear[i];
    rhotot += m[i] * n_linear[i];

    getBulkVel(f[i], v_linear[i], n_linear[i], i);

    T_linear[i] = getTemp(m[i], n_linear[i], v_linear[i], f[i], i);
  }

  // Add electron moment info if needed
  if (ecouple == 1) {

    m_linear[nspec] = m_e;

    // Electron density and velocity
    n_linear[nspec] = 0.0;
    v_linear[nspec][0] = 0.0;
    v_linear[nspec][1] = 0.0;
    v_linear[nspec][2] = 0.0;
    for (i = 0; i < nspec; i++) {
      n_linear[nspec] += Z[i] * n_linear[i];

      v_linear[nspec][0] += rho[i] * v_linear[i][0] / rhotot;
      v_linear[nspec][1] += rho[i] * v_linear[i][1] / rhotot;
      v_linear[nspec][2] += rho[i] * v_linear[i][2] / rhotot;
    }

    // Electron temperature
    T_linear[nspec] = Te;
  }

  // check for blowup, lazily checking first species.
  if (isnan(n_linear[0])) {
    printf("Something weird is going on: What did Jan Say? The Michael Scott "
           "Story. By Michael Scott. With Dwight Schrute.\n NaN detected in "
           "BGK.c\n");
    for (i = 0; i < nspec; i++) {
      printf("%d n: %g v: %g T: %g Z: %g Te: %g\n", i, n_linear[i],
             v_linear[i][0], T_linear[i], Z[i], Te);
    }
    exit(1);
  }

  // Get the collision rates at the current timestep

  // do ij and ji at the same time
  for (i = 0; i < nspec; i++) {
    for (j = i; j < nspec; j++) {

      if (tauFlag == 0) {
        if ((n_linear[i] > EPS_COLL) && (n_linear[j] > EPS_COLL)) {
          getColl(n_linear, T_linear, Te, Z, &nu12, &nu21, i, j);
        }

        else {
          nu12 = 0.0;
          nu21 = 0.0;
        }
      } else {
        printf("Loading collision rates from MD is not implemented for "
               "implicit solve\n");
        exit(1);
      }
      nu_linear[i][j] = nu12;
      nu_linear[j][i] = nu21;
    }
  }

  // Add the electron piece if needed
  if (ecouple == 1) {
    for (j = 0; j < nspec; j++) {
      if (n_linear[j] > EPS_COLL) {
        //-1 says the other species is electrons with a fixed temperature
        getColl(n_linear, T_linear, Te, Z, &nu12, &nu21, -1, j);

        // Electron-ion collisions
        nu_linear[j][nspec] = nu21;
        nu_linear[nspec][j] = nu12;
      } else {
        nu_linear[j][nspec] = 0.0;
        nu_linear[nspec][j] = 0.0;
      }
    }
    nu_linear[nspec][nspec] = 0.0;
  }

  // Now do the BGK update

  int sp, sp2, index;
  double dtnu_over_dt_nui;

  double **vnew = malloc(nspec_linear * sizeof(double *));
  double ***vmix = malloc(nspec_linear * sizeof(double **));
  for (sp = 0; sp < nspec_linear; sp++) {
    vnew[sp] = malloc(3 * sizeof(double));
    vmix[sp] = malloc(nspec_linear * sizeof(double *));
    for (sp2 = 0; sp2 < nspec_linear; sp2++)
      vmix[sp][sp2] = malloc(3 * sizeof(double));
  }
  double Tnew[nspec_linear];
  double **Tmix = malloc(nspec_linear * sizeof(double *));
  for (sp = 0; sp < nspec_linear; sp++)
    Tmix[sp] = malloc(nspec_linear * sizeof(double));

  double *nu_i = malloc(nspec_linear * sizeof(double));

  double *M = malloc(Nv * Nv * Nv * sizeof(double));

  // Set the per species collision rate sum
  for (sp = 0; sp < nspec_linear; sp++) {
    nu_i[sp] = 0;
    for (sp2 = 0; sp2 < nspec_linear; sp2++) {
      nu_i[sp] += nu_linear[sp][sp2];
    }
  }

  // Get new velocity and temperatures

  implicitGetVelocitiesTemperaturesLinear(n_linear, v_linear, T_linear,
                                          nu_linear, m_linear, dt, nspec_linear,
                                          ecouple, vnew, vmix, Tnew, Tmix);

  // Now do the implicit updates of the distribution functions
  for (sp = 0; sp < nspec; sp++) { // nspec limit = Only updating ion species

    // Initial piece of update
    for (index = 0; index < Nv * Nv * Nv; index++)
      f_out[sp][index] = f[sp][index] / (1.0 + dt * nu_i[sp]);

    // Generate the new Maxwellian and add to final result
    for (sp2 = 0; sp2 < nspec_linear; sp2++) {
      dtnu_over_dt_nui = dt * nu_linear[sp][sp2] / (1 + dt * nu_i[sp]);

      GetMaxwell(m_linear[sp], n_linear[sp], vmix[sp][sp2], Tmix[sp][sp2], M,
                 sp);

      for (index = 0; index < Nv * Nv * Nv; index++)
        f_out[sp][index] += dtnu_over_dt_nui * M[index];
    }
  }

  if (TNBFlag > 0) {
    for (sp = 0; sp < nspec; sp++) {

      mu = 0.5 * m[sp];
      if ((mu > 1.6e-24) && (mu < 1.7e-24))
	TNB_DD(f, f_out, sp, rank, TNBFlag, dt, 0.5 * m[sp], n_linear, v_linear, T_linear);

      for (sp2 = sp; sp2 < nspec; sp2++) {
	mu =  m[sp] * m[sp2] / (m[sp] + m[sp2]);
	
	if((mu > 1.8e-24) && (mu < 2.0e-24))
	  TNB_DT(f, f_out, sp, sp2, rank, TNBFlag, dt,
               m[sp] * m[sp2] / (m[sp] + m[sp2]), n_linear, v_linear, T_linear);
      }
    }
  }

  for (sp = 0; sp < nspec; sp++) {
    free(v_linear[sp]);
    free(nu_linear[sp]);
    free(vnew[sp]);
    for (sp2 = 0; sp2 < nspec; sp2++) {
      free(vmix[sp][sp2]);
    }
    free(vmix[sp]);
    free(Tmix[sp]);
  }
  free(n_linear);
  free(v_linear);
  free(T_linear);
  free(nu_linear);

  free(vnew);
  free(vmix);
  free(Tmix);
  free(nu_i);
  free(M);
}

void BGK_norm(double **f, double **f_err, double *Z, double dt, double Te) {

  double ntot, rhotot;

  // Maxwellian params
  double mixU[3];

  double mixT;

  // coll operator stuff
  double nu12, nu21;

  double *f_diff = malloc(Nv * Nv * Nv * sizeof(double));

  int i, j, k;

  // get moments

  ntot = 0.0;
  rhotot = 0.0;
  for (i = 0; i < nspec; i++) {
    n[i] = getDensity(f[i], i);
    rho[i] = m[i] * n[i];
    ntot += n[i];
    rhotot += m[i] * n[i];

    getBulkVel(f[i], v[i], n[i], i);
  }

  // Find temperatures BASED ON INDIVIDUAL SPECIES VELOCITY. Note - result is in
  // eV
  for (i = 0; i < nspec; i++) {
    T[i] = getTemp(m[i], n[i], v[i], f[i], i);
  }

  // check for blowup
  if (isnan(n[0])) {
    printf("Something weird is going on: What did Jan Say? The Michael Scott "
           "Story. By Michael Scott. With Dwight Schrute.\n NaN detected \n");
    for (i = 0; i < nspec; i++) {
      printf("%d n: %g v: %g T: %g Z: %g Te: %g\n", i, n[i], v[i][0], T[i],
             Z[i], Te);
    }
    exit(1);
  }

  // Now generate the BGK operators

  // explicit update

  // initialize the error return vector
  for (i = 0; i < nspec; i++)
    for (j = 0; j < nspec; j++) {
      f_err[i][j] = 0.0;
    }

  ////////////////////////////
  // Find the collision rates//
  ////////////////////////////

  // NOTE: this can probably be refactored/split off for clarity

  // do ij and ji at the same time
  for (i = 0; i < nspec; i++) {
    for (j = i; j < nspec; j++) {

      if (tauFlag == 0) {
        if ((n[i] > EPS_COLL) && (n[j] > EPS_COLL)) {
          getColl(n, T, Te, Z, &nu12, &nu21, i, j);
          collmin = (collmin < 1.0 / nu12) ? collmin : 1.0 / nu12;
          collmin = (collmin < 1.0 / nu21) ? collmin : 1.0 / nu21;

        } else {
          nu12 = 0.0;
          nu21 = 0.0;
        }
      } else if (tauFlag == 1) {
        nu12 = nu_from_MD[i][j];
        nu21 = nu_from_MD[j][i];
      } else {
        printf("Error: set tauflag to 0 or 1\n");
        exit(37);
      }

      // explicit first order update

      if (i == j) {
        if (n[j] >= EPS_COLL) {

          GetMaxwell(m[i], n[i], v[i], T[i], M, i);
#pragma omp parallel for private(k)
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_diff[k] = (M[k] - f[i][k]);

          // calculate error
          f_err[i][j] = norm2_f(f_diff);
        } else
          f_err[i][j] = 0.0;
      } else {
        if (!((n[i] < EPS_COLL) || (n[j] < EPS_COLL))) {

          // Get Maxwell cross terms
          for (k = 0; k < 3; k++) {
            mixU[k] = (rho[i] * nu12 * v[i][k] + rho[j] * nu21 * v[j][k]) /
                      (rho[i] * nu12 + rho[j] * nu21);
          }

          // simplified formulas for mixT
          double vdiff2 = (v[i][0] - v[j][0]) * (v[i][0] - v[j][0]) +
                          (v[i][1] - v[j][1]) * (v[i][1] - v[j][1]) +
                          (v[i][2] - v[j][2]) * (v[i][2] - v[j][2]);

          if (MT_or_TR == 0) {
            mixT =
                (m[j] * T[i] + m[i] * T[j]) / (m[i] + m[j]) +
                (m[i] * m[j]) / (6.0 * (m[i] + m[j])) * ERG_TO_EV_CGS * vdiff2;
          } else {
            mixT = 0.5 * (T[i] + T[j]) + (m[i] * m[j]) / (6.0 * (m[i] + m[j])) *
                                             ERG_TO_EV_CGS * vdiff2;
          }

          if (mixT < 0) {
            printf("Negative mixture temperature! Aborting.\n");
            printf("%d %d %g %g %g %g %g\n", i, j, n[i], n[j], T[i], T[j], Te);
            exit(37);
          }

          GetMaxwell(m[i], n[i], mixU, mixT, M, i);
#pragma omp parallel for private(k)
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_diff[k] = (M[k] - f[i][k]);

          f_err[i][j] = norm2_f(f_diff);

          GetMaxwell(m[j], n[j], mixU, mixT, M, j);
#pragma omp parallel for private(k)
          for (k = 0; k < Nv * Nv * Nv; k++)
            f_diff[k] = (M[k] - f[j][k]);

          f_err[j][i] = norm2_f(f_diff);

        } else {
          f_err[i][j] = 0.0;
          f_err[j][i] = 0.0;
        }
      }
    }
  }

  free(f_diff);
}

void dealloc_BGK() {
  int i;

  for (i = 0; i < 4; i++)
    free(Q[i]);

  free(Q);
  free(M);
}
