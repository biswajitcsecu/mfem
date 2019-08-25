#include "mfem.hpp"
#include <fstream>

using namespace std;
using namespace mfem;

void vel_ex(const Vector &x, Vector &u)
{
   double xi = x(0);
   double yi = x(1);

   u(0) = -cos(M_PI * xi) * sin(M_PI * yi);
   u(1) = sin(M_PI * xi) * cos(M_PI * yi);
}

double p_ex(const Vector &x)
{
   double xi = x(0);
   double yi = x(1);

   return xi + yi - 1.0;
}

void ffun(const Vector &x, Vector &u)
{
   double xi = x(0);
   double yi = x(1);

   u(0) = 1.0 - 2.0 * M_PI * M_PI * cos(M_PI * xi) * sin(M_PI * yi);
   u(1) = 1.0 + 2.0 * M_PI * M_PI * cos(M_PI * yi) * sin(M_PI * xi);
}

int main(int argc, char *argv[])
{
   MPI_Session mpi(argc, argv);

   int num_procs, myid;
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);

   int print_level = 2;
   int serial_ref_levels = 0;
   int order = 2;
   double tol = 1e-8;
   const char *mesh_file = "../data/inline-quad.mesh";

   OptionsParser args(argc, argv);
   args.AddOption(&order, "-o", "--order", "");
   args.AddOption(&tol, "-tol", "--tolerance",
                  "Solver relative tolerance");
   args.AddOption(&print_level, "-pl", "--print-level",
                  "Solver print level");
   args.AddOption(&serial_ref_levels, "-rs", "--serial-ref-levels",
                  "Number of serial refinement levels.");
   args.Parse();
   if (!args.Good())
   {
      if (mpi.Root())
      {
         args.PrintUsage(cout);
      }
      return 1;
   }
   if (mpi.Root())
   {
      args.PrintOptions(cout);
   }

   int vel_order = order;
   int pres_order = order - 1;

   Mesh *mesh = new Mesh(mesh_file);
   int dim = mesh->Dimension();

   for (int l = 0; l < serial_ref_levels; l++)
   {
      mesh->UniformRefinement();
   }

   ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
   delete mesh;

   FiniteElementCollection *vel_fec = new H1_FECollection(vel_order, dim);
   FiniteElementCollection *pres_fec = new H1_FECollection(pres_order);

   ParFiniteElementSpace *vel_fes = new ParFiniteElementSpace(pmesh, vel_fec, dim);
   ParFiniteElementSpace *pres_fes = new ParFiniteElementSpace(pmesh, pres_fec);

   Array<int> ess_tdof_list;
   Array<int> ess_bdr(pmesh->bdr_attributes.Max());
   ess_bdr = 1;
   vel_fes->GetEssentialTrueDofs(ess_bdr, ess_tdof_list);

   Array<int> block_offsets(3);
   block_offsets[0] = 0;
   block_offsets[1] = vel_fes->GetVSize();
   block_offsets[2] = pres_fes->GetVSize();
   block_offsets.PartialSum();

   Array<int> block_trueOffsets(3);
   block_trueOffsets[0] = 0;
   block_trueOffsets[1] = vel_fes->TrueVSize();
   block_trueOffsets[2] = pres_fes->TrueVSize();
   block_trueOffsets.PartialSum();

   int vel_global_vsize = vel_fes->GlobalVSize();
   int pres_global_vsize = pres_fes->GlobalVSize();
   if (myid == 0)
   {
      cout << "Velocity dofs: " << vel_global_vsize << endl;
      cout << "Pressure dofs: " << pres_global_vsize << endl;
   }

   BlockVector x(block_offsets), rhs(block_offsets);
   BlockVector trueX(block_trueOffsets), trueRhs(block_trueOffsets);

   rhs = 0.0;
   x = 0.0;
   trueX = 0.0;
   trueRhs = 0.0;

   VectorFunctionCoefficient uexcoeff(dim, vel_ex);
   VectorFunctionCoefficient fcoeff(dim, ffun);
   FunctionCoefficient pexcoeff(p_ex);

   ParGridFunction *u_gf = new ParGridFunction;
   u_gf->MakeRef(vel_fes, x.GetBlock(0));
   u_gf->ProjectBdrCoefficient(uexcoeff, ess_bdr);

   ParGridFunction *p_gf = new ParGridFunction;
   p_gf->MakeRef(pres_fes, x.GetBlock(1));

   ParLinearForm *fform = new ParLinearForm;
   fform->Update(vel_fes, rhs.GetBlock(0), 0);
   fform->AddDomainIntegrator(new VectorDomainLFIntegrator(fcoeff));
   fform->Assemble();

   ParBilinearForm *sform = new ParBilinearForm(vel_fes);
   sform->AddDomainIntegrator(new VectorDiffusionIntegrator);
   sform->Assemble();
   HypreParMatrix S;
   sform->FormLinearSystem(ess_tdof_list, x.GetBlock(0), rhs.GetBlock(0), S, trueX.GetBlock(0), trueRhs.GetBlock(0));

   ParMixedBilinearForm *dform = new ParMixedBilinearForm(vel_fes, pres_fes);
   dform->AddDomainIntegrator(new VectorDivergenceIntegrator);
   dform->Assemble();
   HypreParMatrix D;
   dform->FormColLinearSystem(ess_tdof_list, x.GetBlock(0), rhs.GetBlock(1), D, trueX.GetBlock(0), trueRhs.GetBlock(1));

   HypreParMatrix *G = D.Transpose();
   (*G) *= -1.0;

   // Flip signs to make system symmetric
   D *= -1.0;
   trueRhs.GetBlock(1) *= -1.0;

   ParBilinearForm *mpform = new ParBilinearForm(pres_fes);
   mpform->AddDomainIntegrator(new MassIntegrator);
   mpform->Assemble();
   mpform->Finalize();
   HypreParMatrix *Mp = mpform->ParallelAssemble();

   BlockOperator *stokesop = new BlockOperator(block_trueOffsets);
   stokesop->SetBlock(0, 0, &S);
   stokesop->SetBlock(0, 1, G);
   stokesop->SetBlock(1, 0, &D);

   HypreSolver *invS = new HypreBoomerAMG(S);
   static_cast<HypreBoomerAMG *>(invS)->SetPrintLevel(0);
   invS->iterative_mode = false;

   HypreDiagScale *invMp = new HypreDiagScale(*Mp);

   BlockDiagonalPreconditioner *stokesprec = new BlockDiagonalPreconditioner(
      block_trueOffsets);
   stokesprec->SetDiagonalBlock(0, invS);
   stokesprec->SetDiagonalBlock(1, invMp);

   MINRESSolver solver(MPI_COMM_WORLD);
   solver.iterative_mode = false;
   solver.SetAbsTol(0.0);
   solver.SetRelTol(tol);
   solver.SetMaxIter(500);
   solver.SetOperator(*stokesop);
   solver.SetPreconditioner(*stokesprec);
   solver.SetPrintLevel(print_level);
   solver.Mult(trueRhs, trueX);

   u_gf->Distribute(&(trueX.GetBlock(0)));
   p_gf->Distribute(&(trueX.GetBlock(1)));

   int order_quad = max(2, 2*order+1);
   const IntegrationRule *irs[Geometry::NumGeom];
   for (int i=0; i < Geometry::NumGeom; ++i)
   {
      irs[i] = &(IntRules.Get(i, order_quad));
   }

   double err_u = u_gf->ComputeL2Error(uexcoeff, irs);
   double norm_u = ComputeGlobalLpNorm(2, uexcoeff, *pmesh, irs);

   double err_p = p_gf->ComputeL2Error(pexcoeff, irs);
   double norm_p = ComputeGlobalLpNorm(2, pexcoeff, *pmesh, irs);

   if (myid == 0)
   {
      cout << "|| u_h - u_ex || = " << err_u << "\n";
      cout << "|| u_h - u_ex || / || u_ex || = " << err_u / norm_u << "\n";
      cout << "|| p_h - p_ex || = " << err_p << "\n";
      cout << "|| p_h - p_ex || / || p_ex || = " << err_p / norm_p << "\n";
   }

   char vishost[] = "localhost";
   int  visport = 19916;
   socketstream u_sock(vishost, visport);
   u_sock << "parallel " << num_procs << " " << myid << "\n";
   u_sock.precision(8);
   u_sock << "solution\n" << *pmesh << *u_gf << "window_title 'velocity'" <<
          "keys Rjlc\n"<< endl;

   socketstream p_sock(vishost, visport);
   p_sock << "parallel " << num_procs << " " << myid << "\n";
   p_sock.precision(8);
   p_sock << "solution\n" << *pmesh << *p_gf << "window_title 'pressure'" <<
          "keys Rjlc\n"<< endl;

   delete vel_fec;
   delete pres_fec;
   delete vel_fes;
   delete pres_fes;
   delete u_gf;
   delete p_gf;
   delete fform;
   delete sform;
   delete dform;
   delete mpform;
   delete stokesop;
   delete stokesprec;
   delete invS;
   delete invMp;
   delete G;
   delete Mp;
   delete pmesh;

   return 0;
}