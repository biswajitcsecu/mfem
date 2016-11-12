// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include "sundials.hpp"

#ifdef MFEM_USE_SUNDIALS

#include "solvers.hpp"
#ifdef MFEM_USE_MPI
#include "hypre.hpp"
#endif

#include <nvector/nvector_serial.h>
#ifdef MFEM_USE_MPI
#include <nvector/nvector_parallel.h>
#include <nvector/nvector_parhyp.h>
#endif

#include <cvode/cvode_impl.h>
#include <cvode/cvode_spgmr.h>

// This just hides a warning (to be removed after it's fixed in SUNDIALS).
#ifdef MSG_TIME_INT
#undef MSG_TIME_INT
#endif

#include <arkode/arkode_impl.h>
#include <arkode/arkode_spgmr.h>

#include <kinsol/kinsol_impl.h>
#include <kinsol/kinsol_spgmr.h>


using namespace std;

namespace mfem
{

double SundialsLinearSolver::GetTimeStep(void *sundials_mem)
{
   return (type == CVODE) ?
          ((CVodeMem)sundials_mem)->cv_gamma :
          ((ARKodeMem)sundials_mem)->ark_gamma;
}

TimeDependentOperator *
SundialsLinearSolver::GetTimeDependentOperator(void *sundials_mem)
{
   void *user_data = (type == CVODE) ?
                     ((CVodeMem)sundials_mem)->cv_user_data :
                     ((ARKodeMem)sundials_mem)->ark_user_data;
   return (TimeDependentOperator *)user_data;
}

static inline SundialsLinearSolver *get_spec(void *ptr)
{
   return static_cast<SundialsLinearSolver *>(ptr);
}

static int LinSysInit(CVodeMem cv_mem)
{
   return get_spec(cv_mem->cv_lmem)->InitSystem(cv_mem);
}

static int LinSysSetup(CVodeMem cv_mem, int convfail,
                       N_Vector ypred, N_Vector fpred, booleantype *jcurPtr,
                       N_Vector vtemp1, N_Vector vtemp2, N_Vector vtemp3)
{
   Vector yp(ypred), fp(fpred), vt1(vtemp1), vt2(vtemp2), vt3(vtemp3);
   return get_spec(cv_mem->cv_lmem)->SetupSystem(cv_mem, convfail, yp, fp,
                                                 *jcurPtr, vt1, vt2, vt3);
}

static int LinSysSolve(CVodeMem cv_mem, N_Vector b, N_Vector weight,
                       N_Vector ycur, N_Vector fcur)
{
   Vector bb(b), w(weight), yc(ycur), fc(fcur);
   return get_spec(cv_mem->cv_lmem)->SolveSystem(cv_mem, bb, w, yc, fc);
}

static int LinSysFree(CVodeMem cv_mem)
{
   return get_spec(cv_mem->cv_lmem)->FreeSystem(cv_mem);
}

static int LinSysInit(ARKodeMem ark_mem)
{
   return get_spec(ark_mem->ark_lmem)->InitSystem(ark_mem);
}

static int LinSysSetup(ARKodeMem ark_mem, int convfail,
                       N_Vector ypred, N_Vector fpred, booleantype *jcurPtr,
                       N_Vector vtemp1, N_Vector vtemp2, N_Vector vtemp3)
{
   Vector yp(ypred), fp(fpred), vt1(vtemp1), vt2(vtemp2), vt3(vtemp3);
   return get_spec(ark_mem->ark_lmem)->SetupSystem(ark_mem, convfail, yp, fp,
                                                   *jcurPtr, vt1, vt2, vt3);
}

static int LinSysSolve(ARKodeMem ark_mem, N_Vector b, N_Vector weight,
                       N_Vector ycur, N_Vector fcur)
{
   Vector bb(b), w(weight), yc(ycur), fc(fcur);
   return get_spec(ark_mem->ark_lmem)->SolveSystem(ark_mem, bb, w, yc, fc);
}

static int LinSysFree(ARKodeMem ark_mem)
{
   return get_spec(ark_mem->ark_lmem)->FreeSystem(ark_mem);
}


const double SundialsSolver::default_rel_tol = 1e-4;
const double SundialsSolver::default_abs_tol = 1e-9;

// static method
int SundialsSolver::ODEMult(realtype t, N_Vector y,
                            N_Vector ydot, void *td_oper)
{
   Vector mfem_y(y), mfem_ydot(ydot);

   // Compute y' = f(t, y).
   TimeDependentOperator *f = static_cast<TimeDependentOperator *>(td_oper);
   f->SetTime(t);
   f->Mult(mfem_y, mfem_ydot);
   return 0;
}

// static method
int SundialsSolver::Mult(N_Vector u, N_Vector fu, void *oper)
{
   Vector mfem_u(u), mfem_fu(fu);

   // Computes the non-linear action F(u).
   static_cast<Operator *>(oper)->Mult(mfem_u, mfem_fu);
   return 0;
}

// static method
int SundialsSolver::GradientMult(N_Vector v, N_Vector Jv, N_Vector u,
                                 booleantype *new_u, void *oper)
{
   Vector mfem_u(u), mfem_v(v), mfem_Jv(Jv);

   Operator &J = static_cast<Operator *>(oper)->GetGradient(mfem_u);
   J.Mult(mfem_v, mfem_Jv);
   return 0;
}

static inline CVodeMem Mem(const CVODESolver *self)
{
   return CVodeMem(self->SundialsMem());
}

CVODESolver::CVODESolver(int lmm, int iter)
{
   // Allocate an empty serial N_Vector wrapper in y.
   y = N_VNewEmpty_Serial(0);
   MFEM_ASSERT(y, "error in N_VNewEmpty_Serial()");

   // Create the solver memory.
   sundials_mem = CVodeCreate(lmm, iter);
   MFEM_ASSERT(sundials_mem, "error in CVodeCreate()");

   SetStepMode(CV_NORMAL);

   // Replace the zero defaults with some positive numbers.
   SetSStolerances(default_rel_tol, default_abs_tol);

   flag = CV_SUCCESS;
}

#ifdef MFEM_USE_MPI

CVODESolver::CVODESolver(MPI_Comm comm, int lmm, int iter)
{
   if (comm == MPI_COMM_NULL)
   {
      // Allocate an empty serial N_Vector wrapper in y.
      y = N_VNewEmpty_Serial(0);
      MFEM_ASSERT(y, "error in N_VNewEmpty_Serial()");
   }
   else
   {
      // Allocate an empty parallel N_Vector wrapper in y.
      y = N_VNewEmpty_Parallel(comm, 0, 0); // calls MPI_Allreduce()
      MFEM_ASSERT(y, "error in N_VNewEmpty_Parallel()");
   }

   // Create the solver memory.
   sundials_mem = CVodeCreate(lmm, iter);
   MFEM_ASSERT(sundials_mem, "error in CVodeCreate()");

   SetStepMode(CV_NORMAL);

   // Replace the zero defaults with some positive numbers.
   SetSStolerances(default_rel_tol, default_abs_tol);

   flag = CV_SUCCESS;
}

#endif // MFEM_USE_MPI

void CVODESolver::SetSStolerances(double reltol, double abstol)
{
   CVodeMem mem = Mem(this);
   // For now store the values in mem:
   mem->cv_reltol = reltol;
   mem->cv_Sabstol = abstol;
   // The call to CVodeSStolerances() is done after CVodeInit() in Init().
}

void CVODESolver::SetLinearSolve(SundialsLinearSolver &ls_spec)
{
   CVodeMem mem = Mem(this);
   MFEM_ASSERT(mem->cv_iter == CV_NEWTON,
               "The function is applicable only to CV_NEWTON iteration type.");

   if (mem->cv_lfree != NULL) { (mem->cv_lfree)(mem); }

   // Set the linear solver function fields in mem.
   // Note that {linit,lsetup,lfree} can be NULL.
   mem->cv_linit  = LinSysInit;
   mem->cv_lsetup = LinSysSetup;
   mem->cv_lsolve = LinSysSolve;
   mem->cv_lfree  = LinSysFree;
   mem->cv_lmem   = &ls_spec;
   mem->cv_setupNonNull = TRUE;
   ls_spec.type = SundialsLinearSolver::CVODE;
}

void CVODESolver::SetStepMode(int itask)
{
   Mem(this)->cv_taskc = itask;
}

void CVODESolver::SetMaxOrder(int max_order)
{
   flag = CVodeSetMaxOrd(sundials_mem, max_order);
   if (flag == CV_ILL_INPUT)
   {
      MFEM_WARNING("CVodeSetMaxOrd() did not change the maximum order!");
   }
}

// Has to copy all fields that can be set by the MFEM interface !!
static inline void cvCopyInit(CVodeMem src, CVodeMem dest)
{
   dest->cv_lmm  = src->cv_lmm;
   dest->cv_iter = src->cv_iter;

   dest->cv_linit  = src->cv_linit;
   dest->cv_lsetup = src->cv_lsetup;
   dest->cv_lsolve = src->cv_lsolve;
   dest->cv_lfree  = src->cv_lfree;
   dest->cv_lmem   = src->cv_lmem;
   dest->cv_setupNonNull = src->cv_setupNonNull;

   dest->cv_reltol  = src->cv_reltol;
   dest->cv_Sabstol = src->cv_Sabstol;

   dest->cv_taskc = src->cv_taskc;
   dest->cv_qmax = src->cv_qmax;
}

void CVODESolver::Init(TimeDependentOperator &f_)
{
   CVodeMem mem = Mem(this);
   CVodeMemRec backup;

   if (mem->cv_MallocDone == TRUE)
   {
      // TODO: preserve more options.
      cvCopyInit(mem, &backup);
      CVodeFree(&sundials_mem);
      sundials_mem = CVodeCreate(backup.cv_lmm, backup.cv_iter);
      MFEM_ASSERT(sundials_mem, "error in CVodeCreate()");
      cvCopyInit(&backup, mem);
   }

   ODESolver::Init(f_);

   // Set actual size and data in the N_Vector y.
   int loc_size = f_.Height();
   if (!Parallel())
   {
      NV_LENGTH_S(y) = loc_size;
      NV_DATA_S(y) = new double[loc_size](); // value-initizalize
   }
   else
   {
#ifdef MFEM_USE_MPI
      long local_size = loc_size, global_size;
      MPI_Allreduce(&local_size, &global_size, 1, MPI_LONG, MPI_SUM,
                    NV_COMM_P(y));
      NV_LOCLENGTH_P(y) = local_size;
      NV_GLOBLENGTH_P(y) = global_size;
      NV_DATA_P(y) = new double[loc_size](); // value-initalize
#endif
   }

   // Call CVodeInit().
   cvCopyInit(mem, &backup);
   flag = CVodeInit(mem, ODEMult, f_.GetTime(), y);
   MFEM_ASSERT(flag >= 0, "CVodeInit() failed!");
   cvCopyInit(&backup, mem);

   // Delete the allocated data in y.
   if (!Parallel())
   {
      delete [] NV_DATA_S(y);
      NV_DATA_S(y) = NULL;
   }
   else
   {
#ifdef MFEM_USE_MPI
      delete [] NV_DATA_P(y);
      NV_DATA_P(y) = NULL;
#endif
   }

   // The TimeDependentOperator pointer, f, will be the user-defined data.
   flag = CVodeSetUserData(sundials_mem, f);
   MFEM_ASSERT(flag >= 0, "CVodeSetUserData() failed!");

   flag = CVodeSStolerances(mem, mem->cv_reltol, mem->cv_Sabstol);
   MFEM_ASSERT(flag >= 0, "CVodeSStolerances() failed!");
}

void CVODESolver::Step(Vector &x, double &t, double &dt)
{
   CVodeMem mem = Mem(this);

   if (!Parallel())
   {
      NV_DATA_S(y) = x.GetData();
      MFEM_VERIFY(NV_LENGTH_S(y) == x.Size(), "");
   }
   else
   {
#ifdef MFEM_USE_MPI
      NV_DATA_P(y) = x.GetData();
      MFEM_VERIFY(NV_LOCLENGTH_P(y) == x.Size(), "");
#endif
   }

   if (mem->cv_nst == 0)
   {
      // Set default linear solver, if not already set.
      if (mem->cv_iter == CV_NEWTON && mem->cv_lsolve == NULL)
      {
         flag = CVSpgmr(sundials_mem, PREC_NONE, 0);
      }
      // Set the actual t0 and y0.
      mem->cv_tn = t;
      N_VScale(ONE, y, mem->cv_zn[0]);
   }

   double tout = t + dt;
   // The actual time integration.
   flag = CVode(sundials_mem, tout, y, &t, mem->cv_taskc);
   MFEM_ASSERT(flag >= 0, "CVode() failed!");

   // Return the last incremental step size.
   dt = mem->cv_hu;
}

void CVODESolver::PrintInfo() const
{
   CVodeMem mem = Mem(this);

   cout <<
        "CVODE:\n  "
        "num steps: " << mem->cv_nst << ", "
        "num evals: " << mem->cv_nfe << ", "
        "num lin setups: " << mem->cv_nsetups << ", "
        "num nonlin sol iters: " << mem->cv_nni << "\n  "
        "last order: " << mem->cv_qu << ", "
        "next order: " << mem->cv_next_q << ", "
        "last dt: " << mem->cv_hu << ", "
        "next dt: " << mem->cv_next_h
        << endl;
}

CVODESolver::~CVODESolver()
{
   N_VDestroy(y);
   CVodeFree(&sundials_mem);
}

static inline ARKodeMem Mem(const ARKODESolver *self)
{
   return ARKodeMem(self->SundialsMem());
}

ARKODESolver::ARKODESolver(bool implicit)
   : use_implicit(implicit), irk_table(-1), erk_table(-1)
{
   // Allocate an empty serial N_Vector wrapper in y.
   y = N_VNewEmpty_Serial(0);
   MFEM_ASSERT(y, "error in N_VNewEmpty_Serial()");

   // Create the solver memory.
   sundials_mem = ARKodeCreate();
   MFEM_ASSERT(sundials_mem, "error in ARKodeCreate()");

   SetStepMode(ARK_NORMAL);

   // Replace the zero defaults with some positive numbers.
   SetSStolerances(default_rel_tol, default_abs_tol);

   flag = ARK_SUCCESS;
}

#ifdef MFEM_USE_MPI
ARKODESolver::ARKODESolver(MPI_Comm comm, bool implicit)
   : use_implicit(implicit), irk_table(-1), erk_table(-1)
{
   if (comm == MPI_COMM_NULL)
   {
      // Allocate an empty serial N_Vector wrapper in y.
      y = N_VNewEmpty_Serial(0);
      MFEM_ASSERT(y, "error in N_VNew_Serial()");
   }
   else
   {
      // Allocate an empty parallel N_Vector wrapper in y.
      y = N_VNewEmpty_Parallel(comm, 0, 0); // calls MPI_Allreduce()
      MFEM_ASSERT(y, "error in N_VNewEmpty_Parallel()");
   }

   // Create the solver memory.
   sundials_mem = ARKodeCreate();
   MFEM_ASSERT(sundials_mem, "error in ARKodeCreate()");

   SetStepMode(ARK_NORMAL);

   // Replace the zero defaults with some positive numbers.
   SetSStolerances(default_rel_tol, default_abs_tol);

   flag = ARK_SUCCESS;
}
#endif

void ARKODESolver::SetSStolerances(realtype reltol, realtype abstol)
{
   ARKodeMem mem = Mem(this);
   // For now store the values in mem:
   mem->ark_reltol  = reltol;
   mem->ark_Sabstol = abstol;
   // The call to ARKodeSStolerances() is done after ARKodeInit() in Init().
}

void ARKODESolver::SetLinearSolve(SundialsLinearSolver &ls_spec)
{
   ARKodeMem mem = Mem(this);
   MFEM_VERIFY(use_implicit,
               "The function is applicable only to implicit time integration.");

   if (mem->ark_lfree != NULL) { mem->ark_lfree(mem); }

   // Tell ARKODE that the Jacobian inversion is custom.
   mem->ark_lsolve_type = 4;
   // Set the linear solver function fields in mem.
   // Note that {linit,lsetup,lfree} can be NULL.
   mem->ark_linit  = LinSysInit;
   mem->ark_lsetup = LinSysSetup;
   mem->ark_lsolve = LinSysSolve;
   mem->ark_lfree  = LinSysFree;
   mem->ark_lmem   = &ls_spec;
   mem->ark_setupNonNull = TRUE;
   ls_spec.type = SundialsLinearSolver::ARKODE;
}

void ARKODESolver::SetStepMode(int itask)
{
   Mem(this)->ark_taskc = itask;
}

void ARKODESolver::SetOrder(int order)
{
   ARKodeMem mem = Mem(this);
   // For now store the values in mem:
   mem->ark_q = order;
   // The call to ARKodeSetOrder() is done after ARKodeInit() in Init().

   flag = ARKodeSetOrder(sundials_mem, order);
   MFEM_ASSERT(flag >= 0, "ARKodeSetOrder() failed!");
}

void ARKODESolver::SetIRKTableNum(int table_num)
{
   // The call to ARKodeSetIRKTableNum() is done after ARKodeInit() in Init().
   irk_table = table_num;
}

void ARKODESolver::SetERKTableNum(int table_num)
{
   // The call to ARKodeSetERKTableNum() is done after ARKodeInit() in Init().
   erk_table = table_num;
}

void ARKODESolver::SetFixedStep(double dt)
{
   flag = ARKodeSetFixedStep(sundials_mem, dt);
   MFEM_ASSERT(flag >= 0, "ARKodeSetFixedStep() failed!");
}

// Copy fields that can be set by the MFEM interface.
static inline void arkCopyInit(ARKodeMem src, ARKodeMem dest)
{
   dest->ark_lsolve_type  = src->ark_lsolve_type;
   dest->ark_linit        = src->ark_linit;
   dest->ark_lsetup       = src->ark_lsetup;
   dest->ark_lsolve       = src->ark_lsolve;
   dest->ark_lfree        = src->ark_lfree;
   dest->ark_lmem         = src->ark_lmem;
   dest->ark_setupNonNull = src->ark_setupNonNull;

   dest->ark_reltol  = src->ark_reltol;
   dest->ark_Sabstol = src->ark_Sabstol;

   dest->ark_taskc     = src->ark_taskc;
   dest->ark_q         = src->ark_q;
   dest->ark_fixedstep = src->ark_fixedstep;
   dest->ark_hin       = src->ark_hin;
}

void ARKODESolver::Init(TimeDependentOperator &f_)
{
   ARKodeMem mem = Mem(this);
   ARKodeMemRec backup;

   // Check if ARKodeInit() has already been called.
   if (mem->ark_MallocDone == TRUE)
   {
      // TODO: preserve more options.
      arkCopyInit(mem, &backup);
      ARKodeFree(&sundials_mem);
      sundials_mem = ARKodeCreate();
      MFEM_ASSERT(sundials_mem, "Error in ARKodeCreate()!");
      arkCopyInit(&backup, mem);
   }

   ODESolver::Init(f_);

   // Set actual size and data in the N_Vector y.
   int loc_size = f_.Height();
   if (!Parallel())
   {
      NV_LENGTH_S(y) = loc_size;
      NV_DATA_S(y) = new double[loc_size](); // value-initialize
   }
   else
   {
#ifdef MFEM_USE_MPI
      long local_size = loc_size, global_size;
      MPI_Allreduce(&local_size, &global_size, 1, MPI_LONG, MPI_SUM,
                    NV_COMM_P(y));
      NV_LOCLENGTH_P(y) = local_size;
      NV_GLOBLENGTH_P(y) = global_size;
      NV_DATA_P(y) = new double[loc_size](); // value-initialize
#endif
   }

   // Call ARKodeInit().
   arkCopyInit(mem, &backup);
   double t = f_.GetTime();
   // TODO: IMEX interface and example.
   flag = (use_implicit) ?
          ARKodeInit(sundials_mem, NULL, ODEMult, t, y) :
          ARKodeInit(sundials_mem, ODEMult, NULL, t, y);
   MFEM_ASSERT(flag >= 0, "CVodeInit() failed!");
   arkCopyInit(&backup, mem);

   // Delete the allocated data in y.
   if (!Parallel())
   {
      delete [] NV_DATA_S(y);
      NV_DATA_S(y) = NULL;
   }
   else
   {
#ifdef MFEM_USE_MPI
      delete [] NV_DATA_P(y);
      NV_DATA_P(y) = NULL;
#endif
   }

   // The TimeDependentOperator pointer, f, will be the user-defined data.
   flag = ARKodeSetUserData(sundials_mem, f);
   MFEM_ASSERT(flag >= 0, "ARKodeSetUserData() failed!");

   flag = ARKodeSStolerances(mem, mem->ark_reltol, mem->ark_Sabstol);
   MFEM_ASSERT(flag >= 0, "CVodeSStolerances() failed!");

   flag = ARKodeSetOrder(sundials_mem, mem->ark_q);
   MFEM_ASSERT(flag >= 0, "ARKodeSetOrder() failed!");

   if (irk_table >= 0) { ARKodeSetIRKTableNum(sundials_mem, irk_table); }
   if (erk_table >= 0) { ARKodeSetERKTableNum(sundials_mem, erk_table); }
}

void ARKODESolver::Step(Vector &x, double &t, double &dt)
{
   ARKodeMem mem = Mem(this);

   if (!Parallel())
   {
      NV_DATA_S(y) = x.GetData();
      MFEM_VERIFY(NV_LENGTH_S(y) == x.Size(), "");
   }
   else
   {
#ifdef MFEM_USE_MPI
      NV_DATA_P(y) = x.GetData();
      MFEM_VERIFY(NV_LOCLENGTH_P(y) == x.Size(), "");
#endif
   }

   if (mem->ark_nst == 0)
   {
      // Set default linear solver, if not already set.
      if (mem->ark_implicit && mem->ark_linit == NULL)
      {
         flag = ARKSpgmr(sundials_mem, PREC_NONE, 0);
      }
      // Set the actual t0 and y0.
      mem->ark_tn = t;
      mem->ark_tnew = t;

      N_VScale(ONE, y, mem->ark_ycur);
   }

   double tout = t + dt;
   // The actual time integration.
   flag = ARKode(sundials_mem, tout, y, &t, mem->ark_taskc);
   MFEM_ASSERT(flag >= 0, "ARKode() failed!");

   // Return the last incremental step size.
   dt = mem->ark_h;
}

void ARKODESolver::PrintInfo() const
{
   ARKodeMem mem = Mem(this);

   cout <<
        "ARKODE:\n  "
        "num steps: " << mem->ark_nst << ", "
        "num evals: " << mem->ark_nfe << ", "
        "num lin setups: " << mem->ark_nsetups << ", "
        "num nonlin sol iters: " << mem->ark_nni << "\n  "
        "method order: " << mem->ark_q << ", "
        "last dt: " << mem->ark_h << ", "
        "next dt: " << mem->ark_next_h
        << endl;
}

ARKODESolver::~ARKODESolver()
{
   N_VDestroy(y);
   ARKodeFree(&sundials_mem);
}

static inline KINMem Mem(const KinSolver *self)
{
   return KINMem(self->SundialsMem());
}

KinSolver::KinSolver(int strategy, bool oper_grad)
   : use_oper_grad(oper_grad), y_scale(NULL), f_scale(NULL)
{
   // Allocate an empty serial N_Vector wrapper in y.
   y = N_VNewEmpty_Serial(0);
   MFEM_ASSERT(y, "Error in N_VNewEmpty_Serial().");
   y_scale = N_VNewEmpty_Serial(0);
   f_scale = N_VNewEmpty_Serial(0);

   sundials_mem = KINCreate();
   MFEM_ASSERT(sundials_mem, "Error in KINCreate().");

   Mem(this)->kin_globalstrategy = strategy;

   flag = KIN_SUCCESS;
}

#ifdef MFEM_USE_MPI

KinSolver::KinSolver(MPI_Comm comm, int strategy, bool oper_grad)
   : use_oper_grad(oper_grad), y_scale(NULL), f_scale(NULL)
{
   if (comm == MPI_COMM_NULL)
   {
      // Allocate an empty serial N_Vector wrapper in y.
      y = N_VNewEmpty_Serial(0);
      MFEM_ASSERT(y, "Error in N_VNewEmpty_Serial().");
      y_scale = N_VNewEmpty_Serial(0);
      f_scale = N_VNewEmpty_Serial(0);
   }
   else
   {
      // Allocate an empty parallel N_Vector wrapper in y.
      y = N_VNewEmpty_Parallel(comm, 0, 0);
      MFEM_ASSERT(y, "Error in N_VNewEmpty_Parallel().");
      y_scale = N_VNewEmpty_Parallel(comm, 0, 0);
      f_scale = N_VNewEmpty_Parallel(comm, 0, 0);
   }

   sundials_mem = KINCreate();
   MFEM_ASSERT(sundials_mem, "Error in KINCreate().");

   Mem(this)->kin_globalstrategy = strategy;

   flag = KIN_SUCCESS;
}

#endif

void KinSolver::SetOperator(const Operator &op)
{
   NewtonSolver::SetOperator(op);
   r = 1.0; c = 1.0;

   // Set actual size and data in the N_Vector y.
   if (!Parallel())
   {
      NV_LENGTH_S(y) = height;
      NV_DATA_S(y) = NULL;
      NV_LENGTH_S(y_scale) = height;
      NV_LENGTH_S(f_scale) = height;
   }
   else
   {
#ifdef MFEM_USE_MPI
      long local_size = height, global_size;
      MPI_Allreduce(&local_size, &global_size, 1, MPI_LONG, MPI_SUM,
                    NV_COMM_P(y));
      NV_LOCLENGTH_P(y) = local_size;
      NV_GLOBLENGTH_P(y) = global_size;
      NV_DATA_P(y) = NULL;
      NV_LOCLENGTH_P(y_scale) = local_size;
      NV_GLOBLENGTH_P(y_scale) = global_size;
      NV_LOCLENGTH_P(f_scale) = local_size;
      NV_GLOBLENGTH_P(f_scale) = global_size;
#endif
   }

   flag = KINInit(sundials_mem, SundialsSolver::Mult, y);
   MFEM_ASSERT(flag >= 0, "KINInit() failed!");

   // Set void pointer to user data.
   flag = KINSetUserData(sundials_mem, const_cast<Operator *>(oper));
   MFEM_ASSERT(flag >= 0, "KINSetUserData() failed!");

   // Set scaled preconditioned GMRES linear solver.
   flag = KINSpgmr(sundials_mem, 0);
   MFEM_ASSERT(flag >= 0, "KINSpgmr() failed!");

   // Define the Jacobian action.
   if (use_oper_grad)
   {
      flag = KINSpilsSetJacTimesVecFn(sundials_mem, GradientMult);
      MFEM_ASSERT(flag >= 0, "KINSpilsSetJacTimesVecFn() failed!");
   }
}

void KinSolver::Mult(const Vector &b, Vector &x) const
{
   // Note r = c = 1.
   Mult(x, r, c);
}

void KinSolver::Mult(Vector &x, Vector &x_scale, Vector &fx_scale) const
{
   KINMem mem = Mem(this);

   flag = KINSetPrintLevel(sundials_mem, print_level);
   MFEM_ASSERT(flag >= 0, "KINSetPrintLevel() failed!");

   flag = KINSetNumMaxIters(sundials_mem, max_iter);
   MFEM_ASSERT(flag >= 0, "KINSetNumMaxIters() failed!");

   flag = KINSetScaledStepTol(sundials_mem, rel_tol);
   MFEM_ASSERT(flag >= 0, "KINSetScaledStepTol() failed!");

   flag = KINSetFuncNormTol(sundials_mem, abs_tol);
   MFEM_ASSERT(flag >= 0, "KINSetFuncNormTol() failed!");

   if (!Parallel())
   {
      NV_DATA_S(y) = x.GetData();
      MFEM_VERIFY(NV_LENGTH_S(y) == x.Size(), "");
      NV_DATA_S(y_scale) = x_scale.GetData();
      NV_DATA_S(f_scale) = fx_scale.GetData();
   }
   else
   {
#ifdef MFEM_USE_MPI
      NV_DATA_P(y) = x.GetData();
      MFEM_VERIFY(NV_LOCLENGTH_P(y) == x.Size(), "");
      NV_DATA_P(y_scale) = x_scale.GetData();
      NV_DATA_P(f_scale) = fx_scale.GetData();
#endif
   }

   flag = KINSol(sundials_mem, y, mem->kin_globalstrategy, y_scale, f_scale);

   converged  = (flag == KIN_MAXITER_REACHED) ? false : true;
   final_iter = mem->kin_nni;
   final_norm = mem->kin_fnorm;
}

KinSolver::~KinSolver()
{
   N_VDestroy(y);
   N_VDestroy(y_scale);
   N_VDestroy(f_scale);
   KINFree(&sundials_mem);
}

} // namespace mfem

#endif
