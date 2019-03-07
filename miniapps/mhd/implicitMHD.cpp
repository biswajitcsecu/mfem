//                                MFEM modified from Example 10
//
// Compile with: make implicitMHD
//
// Sample runs:
//    implicitMHD -m ../../data/beam-quad.mesh -s 3 -r 2 -o 2 -dt 3
//
// Description:  it solves a time dependent resistive MHD problem 
//  10/30/2018 -QT

#include "mfem.hpp"
#include "PDSolver.hpp"
#include <memory>
#include <iostream>
#include <fstream>

using namespace std;
using namespace mfem;


double alpha; //a global value of magnetude for the pertubation
double Lx;  //size of x domain

/** After spatial discretization, the resistive MHD model can be written as a
 *  system of ODEs:
 *     dPsi/dt = M^{-1}*F1,
 *     dw  /dt = M^{-1}*F2,
 *  coupled with two linear systems
 *     j   = M^{-1}*K*Psi 
 *     Phi = K^{-1}*M*w
 *
 *  Class ImplicitMHDOperator represents the right-hand side of the above
 *  system of ODEs. */
class ImplicitMHDOperator : public TimeDependentOperator
{
protected:
   FiniteElementSpace &fespace;

   BilinearForm M, K, DSl, DRe; //mass, stiffness, diffusion with SL and Re
   NonlinearForm Nv, Nb;
   double viscosity, resistivity;

   CGSolver M_solver; // Krylov solver for inverting the mass matrix M
   DSmoother M_prec;  // Preconditioner for the mass matrix M

   CGSolver K_solver; // Krylov solver for inverting the stiffness matrix K
   DSmoother K_prec;  // Preconditioner for the stiffness matrix K

   mutable Vector z; // auxiliary vector 

public:
   ImplicitMHDOperator(FiniteElementSpace &f, Array<int> &ess_bdr, double visc, double resi);

   // Compute the right-hand side of the ODE system.
   void Mult(const Vector &vx, Vector &dvx_dt) const;

   void UpdateJ(const Vector &vx) const;
   void UpdatePhi(const Vector &vx) const;

   // Compute the right-hand side of the ODE system in the predictor step.
   //void MultPre(const Vector &vx, Vector &dvx_dt) const;

   virtual ~ImplicitMHDOperator();
};

//initial condition
void InitialPhi(const Vector &x, double &phi)
{
    phi=0.0;
}

void InitialW(const Vector &x, double &w)
{
    w=0.0;
}

void InitialJ(const Vector &x, double &j)
{
   j =-M_PI*M_PI*(1.0+4.0/Lx/Lx)*alpha*sin(M_PI*x(1))*cos(2.0*M_PI/Lx*x(0));
}

void InitialPsi(const Vector &x, double &psi)
{
   psi =-x(1)+alpha*sin(M_PI*x(1))*cos(2.0*M_PI/Lx*x(0));
}


int main(int argc, char *argv[])
{
   // 1. Parse command-line options.
   const char *mesh_file = "./xperiodic-square.mesh";
   int ref_levels = 2;
   int order = 2;
   int ode_solver_type = 2;
   double t_final = 5.0;
   double dt = 0.0001;
   double visc = 0.0;
   double resi = 0.0;
   alpha = 0.001; 

   bool visualization = true;
   int vis_steps = 1;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&ref_levels, "-r", "--refine",
                  "Number of times to refine the mesh uniformly.");
   args.AddOption(&order, "-o", "--order",
                  "Order (degree) of the finite elements.");
   args.AddOption(&ode_solver_type, "-s", "--ode-solver",
                  "ODE solver: 2 - Brailovskaya,\n\t"
                  " only FE supported 13 - RK3 SSP, 14 - RK4.");
   args.AddOption(&t_final, "-tf", "--t-final",
                  "Final time; start time is 0.");
   args.AddOption(&dt, "-dt", "--time-step",
                  "Time step.");
   args.AddOption(&visc, "-visc", "--viscosity",
                  "Viscosity coefficient.");
   args.AddOption(&resi, "-resi", "--resistivity",
                  "Resistivity coefficient.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&vis_steps, "-vs", "--visualization-steps",
                  "Visualize every n-th timestep.");
   args.Parse();
   if (!args.Good())
   {
      args.PrintUsage(cout);
      return 1;
   }
   args.PrintOptions(cout);

   // 2. Read the mesh from the given mesh file.    
   Mesh *mesh = new Mesh(mesh_file, 1, 1);
   int dim = mesh->Dimension();

   // 3. Define the ODE solver used for time integration. Several implicit
   //    singly diagonal implicit Runge-Kutta (SDIRK) methods, as well as
   //    explicit Runge-Kutta methods are available.
   ODESolver *ode_solver;
   switch (ode_solver_type)
   {
     // Explicit methods FIXME: FE is not working 
     case 1: ode_solver = new ForwardEulerSolver; break;
     case 2: ode_solver = new PDSolver; break; //first order predictor-corrector
     case 3: ode_solver = new RK3SSPSolver; break;
     default:
         cout << "Unknown ODE solver type: " << ode_solver_type << '\n';
         delete mesh;
         return 3;
   }

   // 4. Refine the mesh to increase the resolution.    
   for (int lev = 0; lev < ref_levels; lev++)
   {
      mesh->UniformRefinement();
   }

   // 5. Define the vector finite element spaces representing 
   //  [Psi, Phi, w, j]
   // in block vector bv, with offsets given by the
   //    fe_offset array.
   H1_FECollection fe_coll(order, dim);
   FiniteElementSpace fespace(mesh, &fe_coll); 

   int fe_size = fespace.GetTrueVSize();
   cout << "Number of scalar unknowns: " << fe_size << endl;
   Array<int> fe_offset(5);
   fe_offset[0] = 0;
   fe_offset[1] = fe_size;
   fe_offset[2] = 2*fe_size;
   fe_offset[3] = 3*fe_size;
   fe_offset[4] = 4*fe_size;

   BlockVector bv(fe_offset);
   GridFunction psi, phi, w, j;
   phi.MakeTRef(&fespace, bv.GetBlock(0), 0);
   psi.MakeTRef(&fespace, bv.GetBlock(1), 0);
     w.MakeTRef(&fespace, bv.GetBlock(2), 0);
     j.MakeTRef(&fespace, bv.GetBlock(3), 0);

   // 6. Set the initial conditions, and the boundary conditions
   FunctionCoefficient phiInit(InitialPhi);
   phi.ProjectCoefficient(phiInit);
   phi.SetTrueVector();

   FunctionCoefficient psiInit(InitialPsi);
   psi.ProjectCoefficient(psiInit);
   psi.SetTrueVector();

   FunctionCoefficient wInit(InitialW);
   w.ProjectCoefficient(wInit);
   w.SetTrueVector();

   FunctionCoefficient jInit(InitialJ);
   j.ProjectCoefficient(jInit);
   j.SetTrueVector();

   //this is a periodic boundary condition, so no ess_bdr
   //but may need other things here if not periodic
   Array<int> ess_bdr(fespace.GetMesh()->bdr_attributes.Max());
   ess_bdr = 0;

   // 7. Initialize the MHD operator, the GLVis visualization    
   ImplicitMHDOperator oper(fespace, ess_bdr, visc, resi);

   socketstream vis_phi;
   if (visualization)
   {
      char vishost[] = "localhost";
      int  visport   = 19916;
      vis_phi.open(vishost, visport);
      if (!vis_phi)
      {
         cout << "Unable to connect to GLVis server at "
              << vishost << ':' << visport << endl;
         visualization = false;
         cout << "GLVis visualization disabled.\n";
      }
      else
      {
         vis_phi.precision(8);
         vis_phi << "phi\n" << *mesh << phi;
         vis_phi << "pause\n";
         vis_phi << flush;
         vis_phi << "GLVis visualization paused."
              << " Press space (in the GLVis window) to resume it.\n";
      }
    }

   double t = 0.0;
   oper.SetTime(t);
   ode_solver->Init(oper);

   // 8. Perform time-integration (looping over the time iterations, ti, with a
   //    time-step dt).
   bool last_step = false;
   for (int ti = 1; !last_step; ti++)
   {
      double dt_real = min(dt, t_final - t);

      ode_solver->Step(vx, t, dt_real);

      last_step = (t >= t_final - 1e-8*dt);

      if (last_step || (ti % vis_steps) == 0)
      {
        /*
         double ee = oper.ElasticEnergy(x.GetTrueVector());
         double ke = oper.KineticEnergy(v.GetTrueVector());

         cout << "step " << ti << ", t = " << t << ", EE = " << ee << ", KE = "
              << ke << ", ΔTE = " << (ee+ke)-(ee0+ke0) << endl;
        */

        cout << "step " << ti << ", t = " << t <<endl;

         if (visualization)
         {
            sout << "phi\n" << *mesh << phi << flush;
         }
      }
   }

   // 9. Save the solutions.
   {
      ofstream osol("phi.gf");
      osol.precision(8);
      phi.Save(osol);

      ofstream osol2("current.sol");
      osol2.precision(8);
      j.Save(osol2);

      ofstream osol3("psi.sol");
      osol3.precision(8);
      psi.Save(osol3);

      ofstream osol4("omega.sol");
      osol4.precision(8);
      w.Save(osol4);
   }

   // 10. Free the used memory.
   delete ode_solver;
   delete mesh;

   return 0;
}


//initialization
ImplicitMHDOperator::ImplicitMHDOperator(FiniteElementSpace &f, double visc,double resi)
   : TimeDependentOperator(4*f.GetTrueVSize(), 0.0), fespace(f),
     M(&fespace), K(&fespace), Nv(&fespace), Nb(&fespace),
     viscosity(visc),  resistivity(resi), z(height/4)
{
   const double rel_tol = 1e-8;
   const int skip_zero_entries = 0;
   ConstantCoefficient one(1.0);
   Array<int> ess_tdof_list;
   fespace.GetEssentialTrueDofs(ess_bdr, ess_tdof_list);
   SparseMatrix tmp;    //tmp is not used
   //direct solver for M and K for now

   //mass matrix
   M.AddDomainIntegrator(new MassIntegrator);
   M.Assemble(skip_zero_entries);
   M.FormSystemMatrix(ess_tdof_list, tmp);

   M_solver.iterative_mode = false;
   M_solver.SetRelTol(rel_tol);
   M_solver.SetAbsTol(0.0);
   M_solver.SetMaxIter(30);
   M_solver.SetPrintLevel(0);
   M_solver.SetPreconditioner(M_prec);
   M_solver.SetOperator(M.SpMat());

   //stiffness matrix
   K.AddDomainIntegrator(new DiffusionIntegrator);
   K.Assemble(skip_zero_entries);
   K.FormSystemMatrix(ess_tdof_list, tmp);

   K_solver.iterative_mode = false;
   K_solver.SetRelTol(rel_tol);
   K_solver.SetAbsTol(0.0);
   K_solver.SetMaxIter(30);
   K_solver.SetPrintLevel(0);
   K_solver.SetPreconditioner(K_prec);
   K_solver.SetOperator(K.SpMat()); //this is a real matrix


   //TODO add nonlinear form here???
   H.AddDomainIntegrator(new HyperelasticNLFIntegrator(model));
   H.SetEssentialTrueDofs(ess_tdof_list);


   ConstantCoefficient visc_coeff(viscosity);
   DRe.AddDomainIntegrator(new DiffusionIntegrator(visc_coeff));    
   DRe.Assemble(skip_zero_entries);
   DRe.FormSystemMatrix(ess_tdof_list, tmp);

   ConstantCoefficient resi_coeff(resistivity);
   DSl.AddDomainIntegrator(new DiffusionIntegrator(resi_coeff));    
   DSl.Assemble(skip_zero_entries);
   DSl.FormSystemMatrix(ess_tdof_list, tmp);

}

void ImplicitMHDOperator::Mult(const Vector &vx, Vector &dvx_dt) const
{
   // Create views to the sub-vectors of vx, and dvx_dt
   int sc = height/4;
   Vector phi(vx.GetData() +   0, sc);
   Vector psi(vx.GetData() +  sc, sc);
   Vector   w(vx.GetData() +2*sc, sc);
   Vector   j(vx.GetData() +3*sc, sc);

   Vector dphi_dt(dvx_dt.GetData() +   0, sc);
   Vector dpsi_dt(dvx_dt.GetData() +  sc, sc);
   Vector   dw_dt(dvx_dt.GetData() +2*sc, sc);
   Vector   dj_dt(dvx_dt.GetData() +3*sc, sc);

   dphi_dt=0.0;
   dj_dt=0.0;

   Nv.Mult(psi, z);
   if (resistivity != 0.0)
   {
      DSl.AddMult(psi, z);
   }
   z.Neg(); // z = -z
   M_solver.Mult(z, dpsi_dt);

   Nv.Mult(w, z);
   if (viscosity != 0.0)
   {
      DRe.AddMult(w, z);
   }
   z.Neg(); // z = -z
   Nb.AddMult(j, z);
   M_solver.Mult(z, dw_dt);

}

void ImplicitMHDOperator::UpdateJ(const Vector &vx) const
{
   //the current is J=M^{-1}*K*Psi
   // Create views to the sub-vectors of vx, and dvx_dt
   int sc = height/4;
   Vector phi(vx.GetData() +   0, sc);
   Vector psi(vx.GetData() +  sc, sc);
   Vector   w(vx.GetData() +2*sc, sc);
   Vector   j(vx.GetData() +3*sc, sc);

   K.Mult(psi, z);
   z.Neg(); // z = -z
   M_solver.Mult(z, j);

}

void ImplicitMHDOperator::UpdatePhi(const Vector &vx) const
{
    //Phi=K^{-1}*M*w
   // Create views to the sub-vectors of vx, and dvx_dt
   int sc = height/4;
   Vector phi(vx.GetData() +   0, sc);
   Vector psi(vx.GetData() +  sc, sc);
   Vector   w(vx.GetData() +2*sc, sc);
   Vector   j(vx.GetData() +3*sc, sc);

   M.Mult(w, z);
   z.Neg(); // z = -z
   K_solver.Mult(z, phi);
}


ImplicitMHDOperator::~ImplicitMHDOperator()
{
/* delete pointers */
    //TODO
}

