//                       MFEM Example 1 - Parallel Version
//                                        with Static Condensation
//
// Compile with: make ex1scp
//
// Sample runs:  mpirun -np 4 ex1scp -m ../data/square-disc.mesh
//               mpirun -np 4 ex1scp -m ../data/star.mesh
//               mpirun -np 4 ex1scp -m ../data/escher.mesh
//               mpirun -np 4 ex1scp -m ../data/fichera.mesh
//               mpirun -np 4 ex1scp -m ../data/square-disc-p2.vtk -o 2
//               mpirun -np 4 ex1scp -m ../data/square-disc-p3.mesh -o 3
//               mpirun -np 4 ex1scp -m ../data/square-disc-nurbs.mesh -o -1
//               mpirun -np 4 ex1scp -m ../data/disc-nurbs.mesh -o -1
//               mpirun -np 4 ex1scp -m ../data/pipe-nurbs.mesh -o -1
//               mpirun -np 4 ex1scp -m ../data/ball-nurbs.mesh -o 2
//               mpirun -np 4 ex1scp -m ../data/star-surf.mesh
//               mpirun -np 4 ex1scp -m ../data/square-disc-surf.mesh
//               mpirun -np 4 ex1scp -m ../data/inline-segment.mesh
//
// Description:  This example code demonstrates the use of MFEM to define a
//               simple finite element discretization of the Laplace problem
//               -Delta u = 1 with homogeneous Dirichlet boundary conditions.
//               Specifically, we discretize using a FE space of the specified
//               order, or if order < 1 using an isoparametric/isogeometric
//               space (i.e. quadratic for quadratic curvilinear mesh, NURBS for
//               NURBS mesh, etc.)
//
//               The example highlights the use of mesh refinement, finite
//               element grid functions, as well as linear and bilinear forms
//               corresponding to the left-hand side and right-hand side of the
//               discrete linear system. We also cover the explicit elimination
//               of boundary conditions on all boundary edges, and the optional
//               connection to the GLVis tool for visualization.

#include "mfem.hpp"
#include "../fem/ep.hpp"
#include <fstream>
#include <iostream>

using namespace std;
using namespace mfem;

int main(int argc, char *argv[])
{
   // 1. Initialize MPI.
   int num_procs, myid;
   MPI_Init(&argc, &argv);
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);

   // 2. Parse command-line options.
   const char *mesh_file = "../data/star.mesh";
   int order = 1;
   int sr = 0, pr = 2;
   bool visualization = 1;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree) or -1 for"
                  " isoparametric space.");
   args.AddOption(&sr, "-sr", "--serial-refinement",
                  "Number of serial refinement levels.");
   args.AddOption(&pr, "-pr", "--parallel-refinement",
                  "Number of parallel refinement levels.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.Parse();
   if (!args.Good())
   {
      if (myid == 0)
      {
         args.PrintUsage(cout);
      }
      MPI_Finalize();
      return 1;
   }
   if (myid == 0)
   {
      args.PrintOptions(cout);
   }

   // 3. Read the (serial) mesh from the given mesh file on all processors.  We
   //    can handle triangular, quadrilateral, tetrahedral, hexahedral, surface
   //    and volume meshes with the same code.
   Mesh *mesh;
   ifstream imesh(mesh_file);
   if (!imesh)
   {
      if (myid == 0)
      {
         cerr << "\nCan not open mesh file: " << mesh_file << '\n' << endl;
      }
      MPI_Finalize();
      return 2;
   }
   mesh = new Mesh(imesh, 1, 1);
   imesh.close();
   int dim = mesh->Dimension();

   // 4. Refine the serial mesh on all processors to increase the resolution. In
   //    this example we do 'ref_levels' of uniform refinement. We choose
   //    'ref_levels' to be the largest number that gives a final mesh with no
   //    more than 10,000 elements.
   {
      int ref_levels = sr;
      // (int)floor(log(10000./mesh->GetNE())/log(2.)/dim);
      for (int l = 0; l < ref_levels; l++)
      {
         mesh->UniformRefinement();
      }
   }

   // 5. Define a parallel mesh by a partitioning of the serial mesh. Refine
   //    this mesh further in parallel to increase the resolution. Once the
   //    parallel mesh is defined, the serial mesh can be deleted.
   ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
   delete mesh;
   {
      // int par_ref_levels = 2;
      int par_ref_levels = pr;
      for (int l = 0; l < par_ref_levels; l++)
      {
         pmesh->UniformRefinement();
      }
   }

   // 6. Define a parallel finite element space on the parallel mesh. Here we
   //    use continuous Lagrange finite elements of the specified order. If
   //    order < 1, we instead use an isoparametric/isogeometric space.
   FiniteElementCollection *fec;
   if (order > 0)
   {
      fec = new H1_FECollection(order, dim);
   }
   else if (pmesh->GetNodes())
   {
      fec = pmesh->GetNodes()->OwnFEC();
   }
   else
   {
      fec = new H1_FECollection(order = 1, dim);
   }
   ParFiniteElementSpace *fespace =
      new ParFiniteElementSpace(pmesh, fec, 1, mfem::Ordering::byNODES, true);

   HYPRE_Int  size = fespace->GlobalTrueVSize();
   HYPRE_Int esize = fespace->GlobalTrueExVSize();
   HYPRE_Int psize = size - esize;
   if (myid == 0)
   {
      cout << "Number of unknowns: " << size
           << " (" << esize << " + " << psize << ")" << endl;
   }

   // 7. Set up the parallel linear form b(.) which corresponds to the
   //    right-hand side of the FEM linear system, which in this case is
   //    (1,phi_i) where phi_i are the basis functions in fespace.
   ParLinearForm *b = new ParLinearForm(fespace);
   ConstantCoefficient one(1.0);
   b->AddDomainIntegrator(new DomainLFIntegrator(one));
   b->Assemble();

   // 8. Define the solution vector x as a parallel finite element grid function
   //    corresponding to fespace. Initialize x with initial guess of zero,
   //    which satisfies the boundary conditions.
   ParGridFunction x(fespace);
   x = 0.0;

   // 9. Set up the parallel bilinear form a(.,.) on the finite element space
   //    corresponding to the Laplacian operator -Delta, by adding the Diffusion
   //    domain integrator and imposing homogeneous Dirichlet boundary
   //    conditions. The boundary conditions are implemented by marking all the
   //    boundary attributes from the mesh as essential. After serial and
   //    parallel assembly we extract the corresponding parallel matrix A.
   MPI_Barrier(MPI_COMM_WORLD);
   tic();
   ParBilinearForm *a = new ParBilinearForm(fespace);
   // a->UsePrecomputedSparsity();
   a->AddDomainIntegrator(new DiffusionIntegrator(one));
   a->Assemble();
   a->Finalize();

   // 10. Define the parallel (hypre) matrix and vectors representing a(.,.),
   //     b(.) and the finite element approximation.
   HypreParMatrix *A = a->ParallelAssembleReduced();
   HypreParVector *B = a->RHS_R(*b);
   HypreParVector *X = x.ParallelAverage();

   Array<int> ess_bdr(pmesh->bdr_attributes.Max());
   ess_bdr = 1;
   if ( ess_bdr.Size() > 1 ) { ess_bdr[1] = 0; }

   Array<int> ess_bdr_v, dof_list;
   fespace->GetEssentialExVDofs(ess_bdr,ess_bdr_v);

   for (int i = 0; i < ess_bdr_v.Size(); i++)
   {
      if (ess_bdr_v[i])
      {
         int loctdof = fespace->GetLocalTExDofNumber(i);
         if ( loctdof >= 0 ) { dof_list.Append(loctdof); }
      }
   }

   // do the parallel elimination
   HypreParVector XE(MPI_COMM_WORLD,fespace->GlobalTrueExVSize(),
                     X->GetData(),fespace->GetTrueExDofOffsets());

   A->EliminateRowsCols(dof_list, XE, *B);

   MPI_Barrier(MPI_COMM_WORLD);
   double utime = toc();
   if ( myid == 0 )
     cout << endl << "Assemble time:  " << utime << endl << endl;

   // 11. Define and apply a parallel PCG solver for AX=B with the BoomerAMG
   //     preconditioner from hypre.
   HypreSolver *amg = new HypreBoomerAMG(*A);
   HyprePCG *pcg = new HyprePCG(*A);
   pcg->SetTol(1e-12);
   pcg->SetMaxIter(200);
   pcg->SetPrintLevel(2);
   pcg->SetPreconditioner(*amg);
   pcg->Mult(*B,XE);

   // 12. Extract the parallel grid function corresponding to the finite element
   //     approximation X. This is the local solution on each processor.
   x = *X;
   a->UpdatePrivateDoFs(*b,x);

   delete a;
   delete b;

   // 13. Save the refined mesh and the solution in parallel. This output can
   //     be viewed later using GLVis: "glvis -np <np> -m mesh -g sol".
   {
      ostringstream mesh_name, sol_name;
      mesh_name << "mesh." << setfill('0') << setw(6) << myid;
      sol_name << "sol_sc." << setfill('0') << setw(6) << myid;

      ofstream mesh_ofs(mesh_name.str().c_str());
      mesh_ofs.precision(8);
      pmesh->Print(mesh_ofs);

      ofstream sol_ofs(sol_name.str().c_str());
      sol_ofs.precision(8);
      x.Save(sol_ofs);
   }

   // 14. Send the solution by socket to a GLVis server.
   if (visualization)
   {
      char vishost[] = "localhost";
      int  visport   = 19916;
      socketstream sol_sock(vishost, visport);
      sol_sock << "parallel " << num_procs << " " << myid << "\n";
      sol_sock.precision(8);
      sol_sock << "solution\n" << *pmesh << x << flush;
   }

   // 15. Free the used memory.
   delete pcg;
   delete amg;
   delete X;
   delete B;
   delete A;

   delete fespace;
   if (order > 0)
   {
      delete fec;
   }
   delete pmesh;

   MPI_Finalize();

   return 0;
}
