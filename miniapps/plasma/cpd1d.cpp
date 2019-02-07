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
//
//   -----------------------------------------------------------------------
//   Hertz Miniapp:  Simple Frequency-Domain Electromagnetic Simulation Code
//   -----------------------------------------------------------------------
//
//   Assumes that all sources and boundary conditions oscillate with the same
//   frequency although not necessarily in phase with one another.  This
//   assumptions implies that we can factor out the time dependence which we
//   take to be of the form exp(i omega t).  With these assumptions we can
//   write the Maxwell equations in the form:
//
//   i omega epsilon E = Curl mu^{-1} B - J - sigma E
//   i omega B         = - Curl E
//
//   Which combine to yield:
//
//   Curl mu^{-1} Curl E - omega^2 epsilon E + i omega sigma E = - i omega J
//
//   We discretize this equation with H(Curl) a.k.a Nedelec basis
//   functions.  The curl curl operator must be handled with
//   integration by parts which yields a surface integral:
//
//   (W, Curl mu^{-1} Curl E) = (Curl W, mu^{-1} Curl E)
//               + (W, n x (mu^{-1} Curl E))_{\Gamma}
//
//   or
//
//   (W, Curl mu^{-1} Curl E) = (Curl W, mu^{-1} Curl E)
//               - i omega (W, n x H)_{\Gamma}
//
//   For plane waves
//     omega B = - k x E
//     omega D = k x H, assuming n x k = 0 => n x H = omega epsilon E / |k|
//
//   c = omega/|k|
//
//   (W, Curl mu^{-1} Curl E) = (Curl W, mu^{-1} Curl E)
//               - i omega sqrt{epsilon/mu} (W, E)_{\Gamma}
//
//
// Compile with: make hertz
//
// Sample runs:
//
//   By default the sources and fields are all zero
//     mpirun -np 4 hertz
//
// ./cpd1d -md 0.24 -ne 480 -dbcs '3 5' -s 5 -f 80e6 -maxit 1 -B '0 0 5.4' -w J -slab '0 1 0 0.16' -num '2e20 2e20'
//
//   Current source in a sphere with absorbing boundary conditions
//     mpirun -np 4 hertz -m ../../data/ball-nurbs.mesh -rs 2
//                        -abcs '-1' -f 3e8
//                        -do '-0.3 0.0 0.0 0.3 0.0 0.0 0.1 1 .5 .5'
//
//   Current source in a metal sphere with dielectric and conducting materials
//     mpirun -np 4 hertz -m ../../data/ball-nurbs.mesh -rs 2
//                        -dbcs '-1' -f 3e8
//                        -do '-0.3 0.0 0.0 0.3 0.0 0.0 0.1 1 .5 .5'
//                        -cs '0.0 0.0 -0.5 .2 10'
//                        -ds '0.0 0.0 0.5 .2 10'
//
//   Current source in a metal box
//     mpirun -np 4 hertz -m ../../data/fichera.mesh -rs 3
//                        -dbcs '-1' -f 3e8
//                        -do '-0.5 -0.5 0.0 -0.5 -0.5 1.0 0.1 1 .5 1'
//
//   Current source with a mixture of absorbing and reflecting boundaries
//     mpirun -np 4 hertz -m ../../data/fichera.mesh -rs 3
//                        -do '-0.5 -0.5 0.0 -0.5 -0.5 1.0 0.1 1 .5 1'
//                        -dbcs '4 8 19 21' -abcs '5 18' -f 3e8
//

#include "cold_plasma_dielectric.hpp"
#include "cpd1d_solver.hpp"
#include "../common/mesh_extras.hpp"
#include <fstream>
#include <iostream>
#include <cmath>
#include <complex>

using namespace std;
using namespace mfem;
using namespace mfem::miniapps;
using namespace mfem::plasma;

// Impedance
Coefficient * SetupAdmittanceCoefficient(const Mesh & mesh,
                                         const Array<int> & abcs);

static Vector pw_eta_(0);      // Piecewise impedance values
static Vector pw_eta_inv_(0);  // Piecewise inverse impedance values

// Current Density Function
static Vector slab_params_(0); // Amplitude of x, y, z current source

void slab_current_source(const Vector &x, Vector &j);
void j_src(const Vector &x, Vector &j)
{
   if (slab_params_.Size() > 0)
   {
      slab_current_source(x, j);
   }
}

// Electric Field Boundary Condition: The following function returns zero but
// any function could be used.
void e_bc_r(const Vector &x, Vector &E);
void e_bc_i(const Vector &x, Vector &E);

class ColdPlasmaPlaneWave: public VectorCoefficient
{
public:
   ColdPlasmaPlaneWave(char type,
                       double omega,
                       const Vector & B,
                       const Vector & number,
                       const Vector & charge,
                       const Vector & mass,
                       bool realPart = true);

   void SetCurrentSlab(double Jy, double xJ, double delta, double Lx)
   { Jy_ = Jy; xJ_ = xJ; dx_ = delta, Lx_ = Lx; }

   void Eval(Vector &V, ElementTransformation &T,
             const IntegrationPoint &ip);

private:
   char type_;
   double omega_;
   double Bmag_;
   double Jy_;
   double xJ_;
   double dx_;
   double Lx_;
   bool realPart_;

   const Vector & B_;
   const Vector & numbers_;
   const Vector & charges_;
   const Vector & masses_;

};

//static double freq_ = 1.0e9;

// Mesh Size
static Vector mesh_dim_(0); // x, y, z dimensions of mesh

// Prints the program's logo to the given output stream
// void display_banner(ostream & os);

int main(int argc, char *argv[])
{
   MPI_Session mpi(argc, argv);

   // if ( mpi.Root() ) { display_banner(cout); }

   // Parse command-line options.
   int order = 1;
   int maxit = 100;
   // int serial_ref_levels = 0;
   // int parallel_ref_levels = 0;
   int sol = 2;
   // int nspecies = 2;
   bool herm_conv = false;
   bool visualization = true;
   bool visit = true;

   double freq = 1.0e9;
   const char * wave_type = "R";

   Vector BVec(3);
   BVec = 0.0; BVec(0) = 0.1;

   bool phase_shift = false;
   Vector kVec(3);
   kVec = 0.0;

   Vector numbers;
   Vector charges;
   Vector masses;

   Array<int> abcs;
   Array<int> dbcs;
   int num_elements = 10;

   OptionsParser args(argc, argv);
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree).");
   // args.AddOption(&nspecies, "-ns", "--num-species",
   //               "Number of ion species.");
   args.AddOption(&freq, "-f", "--frequency",
                  "Frequency in Hertz (of course...)");
   args.AddOption(&wave_type, "-w", "--wave-type",
                  "Wave type: 'R' - Right Circularly Polarized, "
                  "'L' - Left Circularly Polarized, "
                  "'O' - Ordinary, 'X' - Extraordinary, "
                  "'J' - Current Sheet (in conjunction with -slab), "
                  "'Z' - Zero");
   args.AddOption(&BVec, "-B", "--magnetic-flux",
                  "Background magnetic flux vector");
   args.AddOption(&kVec[1], "-ky", "--wave-vector-y",
                  "y-Component of wave vector.");
   args.AddOption(&kVec[2], "-kz", "--wave-vector-z",
                  "z-Component of wave vector.");
   args.AddOption(&numbers, "-num", "--number-densites",
                  "Number densities of the various species");
   args.AddOption(&charges, "-q", "--charges",
                  "Charges of the various species "
                  "(in units of electron charge)");
   args.AddOption(&masses, "-m", "--masses",
                  "Masses of the various species (in amu)");
   args.AddOption(&sol, "-s", "--solver",
                  "Solver: 1 - GMRES, 2 - FGMRES w/AMS");
   args.AddOption(&pw_eta_, "-pwz", "--piecewise-eta",
                  "Piecewise values of Impedance (one value per abc surface)");
   args.AddOption(&slab_params_, "-slab", "--slab_params",
                  "Amplitude");
   args.AddOption(&abcs, "-abcs", "--absorbing-bc-surf",
                  "Absorbing Boundary Condition Surfaces");
   args.AddOption(&dbcs, "-dbcs", "--dirichlet-bc-surf",
                  "Dirichlet Boundary Condition Surfaces");
   args.AddOption(&mesh_dim_, "-md", "--mesh_dimensions",
                  "The x, y, z mesh dimensions");
   args.AddOption(&num_elements, "-ne", "--num-elements",
                  "The number of mesh elements in x");
   args.AddOption(&maxit, "-maxit", "--max-amr-iterations",
                  "Max number of iterations in the main AMR loop.");
   args.AddOption(&herm_conv, "-herm", "--hermitian", "-no-herm",
                  "--no-hermitian", "Use convention for Hermitian operators.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&visit, "-visit", "--visit", "-no-visit", "--no-visit",
                  "Enable or disable VisIt visualization.");
   args.Parse();
   if (!args.Good())
   {
      if (mpi.Root())
      {
         args.PrintUsage(cout);
      }
      return 1;
   }
   if (numbers.Size() == 0)
   {
      numbers.SetSize(2);
      numbers[0] = 1.0e19;
      numbers[1] = 1.0e19;
   }
   if (charges.Size() == 0)
   {
      charges.SetSize(2);
      charges[0] = -1.0;
      charges[1] =  1.0;
   }
   if (masses.Size() == 0)
   {
      masses.SetSize(2);
      masses[0] = me_ / u_;
      masses[1] = 2.01410178;
   }
   if (num_elements <= 0)
   {
      num_elements = 10;
   }
   if (mesh_dim_.Size() == 0)
   {
      mesh_dim_.SetSize(3);
      mesh_dim_ = 0.0;
   }
   else if (mesh_dim_.Size() < 3)
   {
      double d0 = mesh_dim_[0];
      double d1 = (mesh_dim_.Size() == 2) ? mesh_dim_[1] : 0.1 * d0;
      mesh_dim_.SetSize(3);
      mesh_dim_[0] = d0;
      mesh_dim_[1] = d1;
      mesh_dim_[2] = d1;
   }
   if (mesh_dim_[0] == 0.0)
   {
      mesh_dim_[0] = 1.0;
      mesh_dim_[1] = 0.1;
      mesh_dim_[2] = 0.1;
   }
   double omega = 2.0 * M_PI * freq;
   if (kVec[1] != 0.0 || kVec[2] != 0.0)
   {
      phase_shift = true;
   }

   if (mpi.Root())
   {
      args.PrintOptions(cout);
   }

   ComplexOperator::Convention conv =
      herm_conv ? ComplexOperator::HERMITIAN : ComplexOperator::BLOCK_SYMMETRIC;

   if (mpi.Root())
   {
      double lam0 = c0_ / freq;
      double Bmag = BVec.Norml2();
      double S = S_cold_plasma(omega, Bmag, numbers, charges, masses);
      double P = P_cold_plasma(omega, numbers, charges, masses);
      double D = D_cold_plasma(omega, Bmag, numbers, charges, masses);
      double R = R_cold_plasma(omega, Bmag, numbers, charges, masses);
      double L = L_cold_plasma(omega, Bmag, numbers, charges, masses);

      cout << "\nConvenient Terms:\n";
      cout << "R = " << R << ",\tL = " << L << endl;
      cout << "S = " << S << ",\tD = " << D << ",\tP = " << P << endl;

      cout << "\nSpecies Properties (number, charge, mass):\n";
      for (int i=0; i<numbers.Size(); i++)
      {
         cout << numbers[i] << '\t' << charges[i] << '\t' << masses[i] << '\n';
      }
      cout << "\nPlasma and Cyclotron Frequencies by Species (GHz):\n";
      for (int i=0; i<numbers.Size(); i++)
      {
         cout << omega_p(numbers[i], charges[i], masses[i]) / (2.0e9 * M_PI)
              << '\t'
              << omega_c(Bmag, charges[i], masses[i]) / (2.0e9 * M_PI) << '\n';
      }

      cout << "\nWavelengths (meters):\n";
      cout << "   Free Space Wavelength: " << lam0 << '\n';
      if (S < D)
      {
         cout << "   Decaying L mode:       " << lam0 / sqrt(D-S) << '\n';
      }
      else
      {
         cout << "   Oscillating L mode:    " << lam0 / sqrt(S-D) << '\n';
      }
      if (S < - D)
      {
         cout << "   Decaying R mode:       " << lam0 / sqrt(-S-D) << '\n';
      }
      else
      {
         cout << "   Oscillating R mode:    " << lam0 / sqrt(S+D) << '\n';
      }
      if (P < 0)
      {
         cout << "   Decaying O mode:       " << lam0 / sqrt(-P) << '\n';
      }
      else
      {
         cout << "   Oscillating O mode:    " << lam0 / sqrt(P) << '\n';
      }
      if ((S * S - D * D) / S < 0)
      {
         cout << "   Decaying X mode:       " << lam0 * sqrt(-S/(S*S-D*D))
              << '\n';
      }
      else
      {
         cout << "   Oscillating X mode:    " << lam0 * sqrt(S/(S*S-D*D))
              << '\n';
      }
      cout << endl;
   }

   // Read the (serial) mesh from the given mesh file on all processors.  We
   // can handle triangular, quadrilateral, tetrahedral, hexahedral, surface
   // and volume meshes with the same code.

   Mesh * mesh = new Mesh(num_elements, 3, 3, Element::HEXAHEDRON, 1,
                          mesh_dim_(0), mesh_dim_(1), mesh_dim_(2));
   {
      vector<Vector> trans(2);
      trans[0].SetSize(3);
      trans[1].SetSize(3);
      trans[0] = 0.0; trans[0][1] = mesh_dim_[1];
      trans[1] = 0.0; trans[1][2] = mesh_dim_[2];
      Mesh * per_mesh = miniapps::MakePeriodicMesh(mesh, trans);
      /*
      ofstream ofs("per_mesh.mesh");
      per_mesh->Print(ofs);
      ofs.close();
      cout << "Chekcing eltrans from mesh" << endl;
      for (int i=0; i<mesh->GetNBE(); i++)
      {
        ElementTransformation * eltrans = mesh->GetBdrElementTransformation(i);
        cout << i
        << '\t' << eltrans->ElementNo
        << '\t' << eltrans->Attribute
        << endl;
      }
      cout << "Chekcing eltrans from per_mesh" << endl;
      for (int i=0; i<per_mesh->GetNBE(); i++)
      {
        ElementTransformation * eltrans = per_mesh->GetBdrElementTransformation(i);
        cout << i
        << '\t' << eltrans->ElementNo
        << '\t' << eltrans->Attribute
        << endl;
      }
      */
      delete mesh;
      mesh = per_mesh;
   }
   if (mpi.Root())
   {
      cout << "Starting initialization." << endl;
   }

   // Ensure that quad and hex meshes are treated as non-conforming.
   mesh->EnsureNCMesh();

   // Define a parallel mesh by a partitioning of the serial mesh. Refine
   // this mesh further in parallel to increase the resolution. Once the
   // parallel mesh is defined, the serial mesh can be deleted.
   ParMesh pmesh(MPI_COMM_WORLD, *mesh);
   delete mesh;
   /*
   {
     for (int i=0; i<pmesh.GetNBE(); i++)
       {
    cout << i << '\t' << pmesh.GetBdrElementBaseGeometry(i)
         << '\t' << pmesh.GetBdrAttribute(i) << endl;
       }
   }
   */
   // If values for Voltage BCs were not set issue a warning and exit
   /*
   if ( ( vbcs.Size() > 0 && kbcs.Size() == 0 ) ||
        ( kbcs.Size() > 0 && vbcs.Size() == 0 ) ||
        ( vbcv.Size() < vbcs.Size() ) )
   {
      if ( mpi.Root() )
      {
         cout << "The surface current (K) boundary condition requires "
              << "surface current boundary condition surfaces (with -kbcs), "
              << "voltage boundary condition surface (with -vbcs), "
              << "and voltage boundary condition values (with -vbcv)."
              << endl;
      }
      return 3;
   }
   */
   VectorConstantCoefficient BCoef(BVec);
   VectorConstantCoefficient kCoef(kVec);
   /*
   double ion_frac = 0.0;
   ConstantCoefficient rhoCoef1(rho1);
   ConstantCoefficient rhoCoef2(rhoCoef1.constant * (1.0 - ion_frac));
   ConstantCoefficient rhoCoef3(rhoCoef1.constant * ion_frac);
   ConstantCoefficient tempCoef(10.0 * q_);
   */
   // H1_ParFESpace H1FESpace(&pmesh, order, pmesh.Dimension());
   ND_ParFESpace HCurlFESpace(&pmesh, order, pmesh.Dimension());
   RT_ParFESpace HDivFESpace(&pmesh, order, pmesh.Dimension());
   L2_ParFESpace L2FESpace(&pmesh, order, pmesh.Dimension());

   ParGridFunction BField(&HDivFESpace);
   // ParGridFunction temperature_gf;
   ParGridFunction density_gf;

   BField.ProjectCoefficient(BCoef);

   // int size_h1 = H1FESpace.GetVSize();
   int size_l2 = L2FESpace.GetVSize();

   Array<int> density_offsets(numbers.Size() + 1);
   // Array<int> temperature_offsets(nspecies + 2);

   density_offsets[0] = 0;
   // temperature_offsets[0] = 0;
   for (int i=1; i<=numbers.Size(); i++)
   {
      density_offsets[i]     = density_offsets[i - 1] + size_l2;
      // temperature_offsets[i + 1] = temperature_offsets[i] + size_h1;
   }

   BlockVector density(density_offsets);
   // BlockVector temperature(temperature_offsets);
   /*
   for (int i=0; i<=nspecies; i++)
   {
      temperature_gf.MakeRef(&H1FESpace, temperature.GetBlock(i));
      temperature_gf.ProjectCoefficient(tempCoef);
   }
   */
   /*
   density_gf.MakeRef(&L2FESpace, density.GetBlock(0));
   density_gf.ProjectCoefficient(rhoCoef1);

   density_gf.MakeRef(&L2FESpace, density.GetBlock(1));
   density_gf.ProjectCoefficient(rhoCoef2);

   density_gf.MakeRef(&L2FESpace, density.GetBlock(2));
   density_gf.ProjectCoefficient(rhoCoef3);
   */
   for (int i=0; i<numbers.Size(); i++)
   {
      ConstantCoefficient rhoCoef(numbers[i]);
      density_gf.MakeRef(&L2FESpace, density.GetBlock(i));
      density_gf.ProjectCoefficient(rhoCoef);
   }

   // Create a coefficient describing the magnetic permeability
   ConstantCoefficient muInvCoef(1.0 / mu0_);

   // Create a coefficient describing the surface admittance
   Coefficient * etaInvCoef = SetupAdmittanceCoefficient(pmesh, abcs);

   // Create tensor coefficients describing the dielectric permittivity
   DielectricTensor epsilon_real(BField, density,
                                 L2FESpace,
                                 omega, charges, masses, true);
   DielectricTensor epsilon_imag(BField, density,
                                 L2FESpace,
                                 omega, charges, masses, false);

   ColdPlasmaPlaneWave EReCoef(wave_type[0], omega, BVec,
                               numbers, charges, masses, true);
   ColdPlasmaPlaneWave EImCoef(wave_type[0], omega, BVec,
                               numbers, charges, masses, false);

   if (wave_type[0] == 'J' && slab_params_.Size() == 5)
   {
      EReCoef.SetCurrentSlab(slab_params_[1], slab_params_[3], slab_params_[4],
                             mesh_dim_[0]);
      EImCoef.SetCurrentSlab(slab_params_[1], slab_params_[3], slab_params_[4],
                             mesh_dim_[0]);
   }
   {
      ParComplexGridFunction EField(&HCurlFESpace);
      EField.ProjectCoefficient(EReCoef, EImCoef);

      char vishost[] = "localhost";
      int  visport   = 19916;

      int Wx = 0, Wy = 0; // window position
      int Ww = 350, Wh = 350; // window size
      int offx = Ww+10, offy = Wh+45; // window offsets

      socketstream sock_Er, sock_Ei, sock_B;
      sock_Er.precision(8);
      sock_Ei.precision(8);
      sock_B.precision(8);

      Wx += 2 * offx;
      VisualizeField(sock_Er, vishost, visport,
                     EField.real(), "Exact Electric Field, Re(E)", Wx, Wy, Ww, Wh);
      Wx += offx;

      VisualizeField(sock_Ei, vishost, visport,
                     EField.imag(), "Exact Electric Field, Im(E)", Wx, Wy, Ww, Wh);
      Wx -= offx;
      Wy += offy;

      VisualizeField(sock_B, vishost, visport,
                     BField, "Background Magnetic Field", Wx, Wy, Ww, Wh);

   }

   // Create the Magnetostatic solver
   CPD1DSolver CPD1D(pmesh, order, omega, (CPD1DSolver::SolverType)sol,
                     conv, epsilon_real, epsilon_imag, muInvCoef, etaInvCoef,
                     (phase_shift) ? &kCoef : NULL,
                     abcs, dbcs,
                     // e_bc_r, e_bc_i,
                     EReCoef, EImCoef,
                     (slab_params_.Size() > 0) ? j_src : NULL, NULL);

   // Initialize GLVis visualization
   if (visualization)
   {
      CPD1D.InitializeGLVis();
   }

   // Initialize VisIt visualization
   VisItDataCollection visit_dc("CPD1D-AMR-Parallel", &pmesh);

   if ( visit )
   {
      CPD1D.RegisterVisItFields(visit_dc);
   }
   if (mpi.Root()) { cout << "Initialization done." << endl; }

   // The main AMR loop. In each iteration we solve the problem on the current
   // mesh, visualize the solution, estimate the error on all elements, refine
   // the worst elements and update all objects to work with the new mesh. We
   // refine until the maximum number of dofs in the Nedelec finite element
   // space reaches 10 million.
   const int max_dofs = 10000000;
   for (int it = 1; it <= maxit; it++)
   {
      if (mpi.Root())
      {
         cout << "\nAMR Iteration " << it << endl;
      }

      // Display the current number of DoFs in each finite element space
      CPD1D.PrintSizes();

      // Assemble all forms
      CPD1D.Assemble();

      // Solve the system and compute any auxiliary fields
      CPD1D.Solve();

      // Compute error
      double glb_error = CPD1D.GetError();
      if (mpi.Root())
      {
         cout << "Global L2 Error " << glb_error << endl;
      }

      // Determine the current size of the linear system
      int prob_size = CPD1D.GetProblemSize();

      // Write fields to disk for VisIt
      if ( visit )
      {
         CPD1D.WriteVisItFields(it);
      }

      // Send the solution by socket to a GLVis server.
      if (visualization)
      {
         CPD1D.DisplayToGLVis();
      }

      if (mpi.Root())
      {
         cout << "AMR iteration " << it << " complete." << endl;
      }

      // Check stopping criteria
      if (prob_size > max_dofs)
      {
         if (mpi.Root())
         {
            cout << "Reached maximum number of dofs, exiting..." << endl;
         }
         break;
      }
      if ( it == maxit )
      {
         break;
      }

      // Wait for user input. Ask every 10th iteration.
      char c = 'c';
      if (mpi.Root() && (it % 10 == 0))
      {
         cout << "press (q)uit or (c)ontinue --> " << flush;
         cin >> c;
      }
      MPI_Bcast(&c, 1, MPI_CHAR, 0, MPI_COMM_WORLD);

      if (c != 'c')
      {
         break;
      }

      // Estimate element errors using the Zienkiewicz-Zhu error estimator.
      Vector errors(pmesh.GetNE());
      CPD1D.GetErrorEstimates(errors);

      double local_max_err = errors.Max();
      double global_max_err;
      MPI_Allreduce(&local_max_err, &global_max_err, 1,
                    MPI_DOUBLE, MPI_MAX, pmesh.GetComm());

      // Refine the elements whose error is larger than a fraction of the
      // maximum element error.
      const double frac = 0.5;
      double threshold = frac * global_max_err;
      if (mpi.Root()) { cout << "Refining ..." << endl; }
      pmesh.RefineByError(errors, threshold);

      // Update the magnetostatic solver to reflect the new state of the mesh.
      CPD1D.Update();

      if (pmesh.Nonconforming() && mpi.WorldSize() > 1 && false)
      {
         if (mpi.Root()) { cout << "Rebalancing ..." << endl; }
         pmesh.Rebalance();

         // Update again after rebalancing
         CPD1D.Update();
      }
   }

   // Send the solution by socket to a GLVis server.
   if (visualization)
   {
      CPD1D.DisplayAnimationToGLVis();
   }

   // delete epsCoef;
   // delete muInvCoef;
   // delete sigmaCoef;

   return 0;
}

// Print the CPD1D ascii logo to the given ostream
/*
void display_banner(ostream & os)
{
   os << "     ____  ____              __           " << endl
      << "    /   / /   / ____________/  |_________ " << endl
      << "   /   /_/   /_/ __ \\_  __ \\   __\\___   / " << endl
      << "  /   __    / \\  ___/|  | \\/|  |  /   _/  " << endl
      << " /___/ /_  /   \\___  >__|   |__| /_____ \\ " << endl
      << "         \\/        \\/                  \\/ " << endl << flush;
}
*/
// The Admittance is an optional coefficient defined on boundary surfaces which
// can be used in conjunction with absorbing boundary conditions.
Coefficient *
SetupAdmittanceCoefficient(const Mesh & mesh, const Array<int> & abcs)
{
   Coefficient * coef = NULL;

   if ( pw_eta_.Size() > 0 )
   {
      MFEM_VERIFY(pw_eta_.Size() == abcs.Size(),
                  "Each impedance value must be associated with exactly one "
                  "absorbing boundary surface.");

      pw_eta_inv_.SetSize(mesh.bdr_attributes.Size());

      if ( abcs[0] == -1 )
      {
         pw_eta_inv_ = 1.0 / pw_eta_[0];
      }
      else
      {
         pw_eta_inv_ = 0.0;

         for (int i=0; i<pw_eta_.Size(); i++)
         {
            pw_eta_inv_[abcs[i]-1] = 1.0 / pw_eta_[i];
         }
      }
      coef = new PWConstCoefficient(pw_eta_inv_);
   }

   return coef;
}

void slab_current_source(const Vector &x, Vector &j)
{
   MFEM_ASSERT(x.Size() == 3, "current source requires 3D space.");

   j.SetSize(x.Size());
   j = 0.0;

   double width = slab_params_(4);
   // double height = 1.0 / width;
   double half_x_l = slab_params_(3) - 0.5 * width;
   double half_x_r = slab_params_(3) + 0.5 * width;

   if (x(0) <= half_x_r && x(0) >= half_x_l)
   {
      j(0) = slab_params_(0);
      j(1) = slab_params_(1);
      j(2) = slab_params_(2);
   }
   // j *= height;
}

void e_bc_r(const Vector &x, Vector &E)
{
   E.SetSize(3);
   E = 0.0;

}

void e_bc_i(const Vector &x, Vector &E)
{
   E.SetSize(3);
   E = 0.0;
}

ColdPlasmaPlaneWave::ColdPlasmaPlaneWave(char type,
                                         double omega,
                                         const Vector & B,
                                         const Vector & number,
                                         const Vector & charge,
                                         const Vector & mass,
                                         bool realPart)
   : VectorCoefficient(3),
     type_(type),
     omega_(omega),
     Bmag_(B.Norml2()),
     Jy_(0.0),
     xJ_(0.5),
     Lx_(1.0),
     realPart_(realPart),
     B_(B),
     numbers_(number),
     charges_(charge),
     masses_(mass)
{}

void ColdPlasmaPlaneWave::Eval(Vector &V, ElementTransformation &T,
                               const IntegrationPoint &ip)
{
   V.SetSize(3);

   double x_data[3];
   Vector x(x_data, 3);
   T.Transform(ip, x);

   switch (type_)
   {
      case 'L':
      {
         double S = S_cold_plasma(omega_, Bmag_, numbers_, charges_, masses_);
         double D = D_cold_plasma(omega_, Bmag_, numbers_, charges_, masses_);

         bool osc = S - D > 0.0;
         double kL = omega_ * sqrt(fabs(S-D)) / c0_;

         if (realPart_)
         {
            V[0] = 0.0;
            V[1] = osc ?  sin(kL * x[0]) : 0.0;
            V[2] = osc ?  cos(kL * x[0]) : exp(-kL * x[0]);
         }
         else
         {
            V[0] = 0.0;
            V[1] = osc ?  cos(kL * x[0]) : exp(-kL * x[0]);
            V[2] = osc ? -sin(kL * x[0]) : 0.0;
         }
      }
      break;
      case 'R':
      {
         double S = S_cold_plasma(omega_, Bmag_, numbers_, charges_, masses_);
         double D = D_cold_plasma(omega_, Bmag_, numbers_, charges_, masses_);

         bool osc = S + D > 0.0;
         double kR = omega_ * sqrt(fabs(S+D)) / c0_;

         if (realPart_)
         {
            V[0] = 0.0;
            V[1] = osc ? -sin(kR * x[0]) : 0.0;
            V[2] = osc ?  cos(kR * x[0]) : exp(-kR * x[0]);
         }
         else
         {
            V[0] = 0.0;
            V[1] = osc ? -cos(kR * x[0]) : -exp(-kR * x[0]);
            V[2] = osc ? -sin(kR * x[0]) : 0.0;
         }
      }
      break;
      case 'O':
      {
         double P = P_cold_plasma(omega_, numbers_, charges_, masses_);

         bool osc = P > 0.0;
         double kO = omega_ * sqrt(fabs(P)) / c0_;

         if (realPart_)
         {
            V[0] = 0.0;
            V[1] = osc ? cos(kO * x[0]) : exp(-kO * x[0]);
            V[2] = 0.0;
         }
         else
         {
            V[0] = 0.0;
            V[1] = osc ? -sin(kO * x[0]) : 0.0;
            V[2] = 0.0;
         }
      }
      break;
      case 'X':
      {
         double S = S_cold_plasma(omega_, Bmag_, numbers_, charges_, masses_);
         double D = D_cold_plasma(omega_, Bmag_, numbers_, charges_, masses_);

         bool osc = (S * S - D * D) / S > 0.0;
         double kE = omega_ * sqrt(fabs((S * S - D * D) / S)) / c0_;

         if (realPart_)
         {
            V[0] = osc ? -D * sin(kE * x[0]) : 0.0;
            V[1] = 0.0;
            V[2] = osc ?  S * cos(kE * x[0]) : S * exp(-kE * x[0]);
         }
         else
         {
            V[0] = osc ? -D * cos(kE * x[0]) : -D * exp(-kE * x[0]);
            V[1] = 0.0;
            V[2] = osc ? -S * sin(kE * x[0]) : 0.0;
         }
         V /= sqrt(S * S + D * D);
      }
      break;
      case 'J':
      {
         double S = S_cold_plasma(omega_, Bmag_, numbers_, charges_, masses_);
         double D = D_cold_plasma(omega_, Bmag_, numbers_, charges_, masses_);

         bool osc = (S * S - D * D) / S > 0.0;
         double kE = omega_ * sqrt(fabs((S * S - D * D) / S)) / c0_;

         double (*sfunc)(double) = osc ?
                                   static_cast<double (*)(double)>(&sin) :
                                   static_cast<double (*)(double)>(&sinh);
         double (*cfunc)(double) = osc ?
                                   static_cast<double (*)(double)>(&cos) :
                                   static_cast<double (*)(double)>(&cosh);

         double skL   = (*sfunc)(kE * Lx_);
         double csckL = 1.0 / skL;

         if (realPart_)
         {
            V[0] = D / S;
            V[1] = 0.0;
            V[2] = 0.0;
         }
         else
         {
            V[0] = 0.0;
            V[1] = -1.0;
            V[2] = 0.0;
         }

         if (x[0] <= xJ_ - 0.5 * dx_)
         {
            double skx    = (*sfunc)(kE * x[0]);
            double skLxJ  = (*sfunc)(kE * (Lx_ - xJ_));
            double skd    = (*sfunc)(kE * 0.5 * dx_);
            double a = skx * skLxJ * skd;

            V *= omega_ * mu0_ * Jy_ * a * csckL / (kE * kE);
            if (!osc) { V *= -1.0; }
         }
         else if (x[0] <= xJ_ + 0.5 * dx_)
         {
            double skx      = (*sfunc)(kE * x[0]);
            double skLx     = (*sfunc)(kE * (Lx_ - x[0]));
            double ckxJmd   = (*cfunc)(kE * (xJ_ - 0.5 * dx_));
            double ckLxJmd  = (*cfunc)(kE * (Lx_ - xJ_ - 0.5 * dx_));
            double a = skx * ckLxJmd + skLx * ckxJmd - skL;

            V *= omega_ * mu0_ * Jy_ * a * csckL / (kE * kE);
         }
         else
         {
            double skLx = (*sfunc)(kE * (Lx_ - x[0]));
            double skxJ = (*sfunc)(kE * xJ_);
            double skd  = (*sfunc)(kE * 0.5 * dx_);
            double a = skLx * skxJ * skd;

            V *= omega_ * mu0_ * Jy_ * a * csckL / (kE * kE);
            if (!osc) { V *= -1.0; }
         }
      }
      break;
      case 'Z':
         V = 0.0;
         break;
   }
}
