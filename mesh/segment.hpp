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

#ifndef MFEM_SEGMENT
#define MFEM_SEGMENT

#include "../config/config.hpp"
#include "element.hpp"

namespace mfem
{

/// Data type line segment element
class Segment : public Element
{

public:
   typedef Geometry::Constants<Geometry::SEGMENT> geom_t;
   static const size_t NUM_INDICES = 2;

   Segment() : Element(Geometry::SEGMENT, NULL, 2, NULL) { }
   Segment(int_ptr_pair p) : Element(Geometry::SEGMENT, p.first, 2, p.second) { }

   /// Constructs triangle by specifying the indices and the attribute.
   Segment(const int *ind, int attr = 1, int_ptr_pair = int_ptr_pair(NULL, NULL));

   /// Constructs triangle by specifying the indices and the attribute.
   Segment(int ind1, int ind2, int attr = 1, int_ptr_pair = int_ptr_pair(NULL, NULL));

   /// Set the indices the element according to the input.
   virtual void SetVertices(const int *ind);

   /// Return element's type.
   virtual int GetType() const { return Element::SEGMENT; }

   /// Returns the indices of the element's  vertices.
   virtual void GetVertices(Array<int> &v) const;

   virtual int *GetVertices() { return indices; }

   virtual int GetNVertices() const { return 2; }

   virtual int GetNEdges() const { return (0); }

   virtual const int *GetEdgeVertices(int ei) const { return NULL; }

   virtual int GetNFaces(int &nFaceVertices) const
   { nFaceVertices = 0; return 0; }

   virtual const int *GetFaceVertices(int fi) const { return NULL; }

   virtual Element *Duplicate(Mesh *m) const
   { return new Segment(indices, attribute); }

   virtual ~Segment() { }
};

extern Linear1DFiniteElement SegmentFE;

}

#endif
