// Copyright (c) 2010-2021, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

// Implementation of data types for distributive relaxation smoother

#include "simple_amg.hpp"
#include <unordered_set>
#include <set>
#include <unordered_map>
#include "solvers.hpp"
#include <vector>

namespace mfem
{

void SimpleAMG::FormRestriction()
{
   int n = A.Height();

   const int *I = A.GetI();
   const int *J = A.GetJ();
   const double *data = A.GetData();

   // Determine neighbors of each DOF
   Array<int> priorities(n);
   std::vector<std::unordered_set<int>> neighbors(n);
   for (int i = 0; i < n; ++i)
   {
      priorities[i] = 0;
      neighbors[i] = std::unordered_set<int>();
      for (int j = I[i]; j < I[i+1]; ++j)
      {
         if (data[j] != 0.0 && J[j] != i)
         {
            priorities[i]++;
            neighbors[i].insert(J[j]);
         }
      }
   }

   // Create priorty queue for DOFS
   std::set<std::pair<int, int>> queue;
   for (int i = 0; i < n; ++i)
   {
      queue.insert(std::make_pair(priorities[i], i));
   }

   // Determine coarse points
   std::unordered_set<int> coarse_points;
   while (!queue.empty())
   {
      auto top = queue.rbegin();
      std::pair<int, int> priority_and_v = *top;
      queue.erase(*top);

      const int v = priority_and_v.second;
      coarse_points.insert(v);
      priorities[v] = -1;

      for (int neigh : neighbors[v])
      {
         if (priorities[neigh] >= 0)
         {
            queue.erase(std::make_pair(priorities[neigh], neigh));
            priorities[neigh] = -1;

            for (int nneigh : neighbors[neigh])
            {
               if (priorities[nneigh] >= 0)
               {
                  queue.erase(std::make_pair(priorities[nneigh], nneigh));
                  priorities[nneigh]++;
                  queue.insert(std::make_pair(priorities[nneigh], nneigh));
               }
            }
         }
      }
   }

   // Number each coarse point
   std::unordered_map<int,int> coarse_to_label;
   int j = 0;
   for (int i : coarse_points)
   {
      coarse_to_label[i] = j;
      j++;
   }

   // Assemble prolongator
   int coarse_vertices = coarse_points.size();
   SparseMatrix R_(coarse_vertices, n);

   for (int i = 0; i < n; ++i)
   {
      if (coarse_points.count(i))
      {
         R_.Add(coarse_to_label[i], i, 1.0);
      }
      else
      {
         std::unordered_set<int> coarse_neighbors = neighbors[i];

         auto itr = coarse_neighbors.begin();
         while (itr != coarse_neighbors.end())
         {
            int neigh = *itr;
            if (!coarse_points.count(neigh))
            {
               itr = coarse_neighbors.erase(itr);
            }
            else
            {
               itr++;
            }
         }

         int num_coarse_neigh = coarse_neighbors.size();

         MFEM_VERIFY(num_coarse_neigh > 0,
                     "Every DOF must have at least one coarse neighbor");

         itr = coarse_neighbors.begin();
         for (int j = 0; j < num_coarse_neigh; ++j)
         {
            R_.Add(coarse_to_label[*itr], i, 1.0 / num_coarse_neigh);
            itr++;
         }
      }
   }

   R_.Finalize();
   R_.BuildTranspose();
   R.Swap(R_);
}

SimpleAMG::SimpleAMG(const SparseMatrix &A_, Solver &smoother_, MPI_Comm comm,
                     bool two_level) : A(A_), smoother(smoother_)
{
   FormRestriction();
   Ac.reset(RAP(A, R));

   if (two_level)
   {
#ifdef MFEM_USE_SUITESPARSE
      UMFPackSolver *umf = new UMFPackSolver;
      umf->Control[UMFPACK_ORDERING] = UMFPACK_ORDERING_METIS;
      umf->SetOperator(*Ac);
      coarse_solver.reset(umf);
      Ac_par = NULL;
#else
      MFEM_ABORT("SimpleAMG requires UMFPack in order to do two-level method");
#endif
   }
   else
   {
      row_starts[0] = 0;
      row_starts[1] = Ac->Height();
      Ac_par.reset(new HypreParMatrix(comm, Ac->Height(), row_starts, Ac.get()));
      coarse_solver.reset(new HypreBoomerAMG(*Ac_par));
   }
}

void SimpleAMG::Mult(const Vector &x, Vector &y) const
{
   z.SetSize(x.Size());
   r_c.SetSize(R.Height());
   e_c.SetSize(R.Height());

   // Pre-smoothing
   smoother.Mult(x, y);
   // Compute residual
   r = x;
   A.AddMult(y, r, -1.0);
   // Restrict to coarse space
   R.Mult(r, r_c);
   // Solve coarse system
   coarse_solver->Mult(r_c, e_c);
   // Prolongate error
   R.AddMultTranspose(e_c, y);
   // Compute residual
   r = x;
   A.AddMult(y, r, -1.0);
   // TODO: if we implemented AddMult for the smoother, this temporary can be
   // eliminated.
   smoother.Mult(r, z);
   add(y, 1.0, z, y);
}

} // namespace mfem