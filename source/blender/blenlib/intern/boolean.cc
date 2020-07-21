/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <algorithm>
#include <fstream>
#include <iostream>

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_delaunay_2d.h"
#include "BLI_hash.hh"
#include "BLI_map.hh"
#include "BLI_math.h"
#include "BLI_math_mpq.hh"
#include "BLI_mesh_intersect.hh"
#include "BLI_mpq3.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_stack.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "BLI_boolean.hh"

namespace blender::meshintersect {

/* Edge as two Vertp's, in a canonical order (lower vert id first).
 * We use the Vert id field for hashing to get algorithms
 * that yield predictable results from run-to-run and machine-to-machine.
 */
class Edge {
  Vertp v_[2]{nullptr, nullptr};

 public:
  Edge() = default;
  Edge(Vertp v0, Vertp v1)
  {
    if (v0->id <= v1->id) {
      v_[0] = v0;
      v_[1] = v1;
    }
    else {
      v_[0] = v1;
      v_[1] = v0;
    }
  }

  Vertp v0() const
  {
    return v_[0];
  }

  Vertp v1() const
  {
    return v_[1];
  }

  Vertp operator[](int i) const
  {
    return v_[i];
  }

  bool operator==(Edge other) const
  {
    return v_[0]->id == other.v_[0]->id && v_[1]->id == other.v_[1]->id;
  }

  uint64_t hash() const
  {
    constexpr uint64_t h1 = 33;
    uint64_t v0hash = DefaultHash<int>{}(v_[0]->id);
    uint64_t v1hash = DefaultHash<int>{}(v_[1]->id);
    return v0hash ^ (v1hash * h1);
  }
};

static std::ostream &operator<<(std::ostream &os, const Edge &e)
{
  if (e.v0() == nullptr) {
    BLI_assert(e.v1() == nullptr);
    os << "(null,null)";
  }
  else {
    os << "(" << e.v0() << "," << e.v1() << ")";
  }
  return os;
}

static std::ostream &operator<<(std::ostream &os, const Span<int> &a)
{
  for (int i : a.index_range()) {
    os << a[i];
    if (i != a.size() - 1) {
      os << " ";
    }
  }
  return os;
}

static std::ostream &operator<<(std::ostream &os, const Array<int> &iarr)
{
  os << Span<int>(iarr);
  return os;
}

/* Holds information about topology of a Mesh that is all triangles. */
class TriMeshTopology {
  /* Triangles that contain a given Edge (either order). */
  Map<Edge, Vector<int> *> edge_tri_;
  /* Edges incident on each vertex. */
  Map<Vertp, Vector<Edge>> vert_edges_;

 public:
  TriMeshTopology(const Mesh &tm);
  TriMeshTopology(const TriMeshTopology &other) = delete;
  TriMeshTopology(const TriMeshTopology &&other) = delete;
  ~TriMeshTopology();

  /* If e is manifold, return index of the other triangle (not t) that has it. Else return
   * NO_INDEX. */
  int other_tri_if_manifold(Edge e, int t) const
  {
    if (edge_tri_.contains(e)) {
      auto *p = edge_tri_.lookup(e);
      if (p->size() == 2) {
        return ((*p)[0] == t) ? (*p)[1] : (*p)[0];
      }
    }
    return NO_INDEX;
  }

  /* Which triangles share edge e (in either orientation)? */
  const Vector<int> *edge_tris(Edge e) const
  {
    return edge_tri_.lookup_default(e, nullptr);
  }

  /* Which edges are incident on the given vertex?
   * We assume v has some incident edges.
   */
  const Vector<Edge> &vert_edges(Vertp v) const
  {
    return vert_edges_.lookup(v);
  }
};

TriMeshTopology::TriMeshTopology(const Mesh &tm)
{
  const int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "TriMeshTopology construction\n";
  }
  /* If everything were manifold, F+V-E=2 and E=3F/2.
   * So an likely overestimate, allowing for non-manifoldness, is E=2F and V=F.
   */
  const int estimate_num_edges = 2 * tm.face_size();
  const int estimate_num_verts = tm.face_size();
  edge_tri_.reserve(estimate_num_edges);
  vert_edges_.reserve(estimate_num_verts);
  for (int t : tm.face_index_range()) {
    const Face &tri = *tm.face(t);
    BLI_assert(tri.is_tri());
    for (int i = 0; i < 3; ++i) {
      Vertp v = tri[i];
      Vertp vnext = tri[(i + 1) % 3];
      Edge e(v, vnext);
      Vector<Edge> *edges = vert_edges_.lookup_ptr(v);
      if (edges == nullptr) {
        vert_edges_.add_new(v, Vector<Edge>());
        edges = vert_edges_.lookup_ptr(v);
        BLI_assert(edges != nullptr);
      }
      edges->append_non_duplicates(e);
      auto createf = [t](Vector<int> **pvec) { *pvec = new Vector<int>{t}; };
      auto modifyf = [t](Vector<int> **pvec) { (*pvec)->append_non_duplicates(t); };
      this->edge_tri_.add_or_modify(Edge(v, vnext), createf, modifyf);
    }
  }
  /* Debugging. */
  if (dbg_level > 0) {
    std::cout << "After TriMeshTopology construction\n";
    for (auto item : edge_tri_.items()) {
      std::cout << "tris for edge " << item.key << ": " << *item.value << "\n";
      constexpr bool print_stats = false;
      if (print_stats) {
        edge_tri_.print_stats();
      }
    }
    for (auto item : vert_edges_.items()) {
      std::cout << "edges for vert " << item.key << ":\n";
      for (const Edge &e : item.value) {
        std::cout << "  " << e << "\n";
      }
      std::cout << "\n";
    }
  }
}

TriMeshTopology::~TriMeshTopology()
{
  for (const Vector<int> *vec : edge_tri_.values()) {
    delete vec;
  }
}

/* A Patch is a maximal set of triangles that share manifold edges only. */
class Patch {
  Vector<int> tri_; /* Indices of triangles in the Patch. */

 public:
  Patch() = default;

  void add_tri(int t)
  {
    tri_.append(t);
  }

  const Vector<int> &tri() const
  {
    return tri_;
  }

  int tot_tri() const
  {
    return tri_.size();
  }

  int tri(int i) const
  {
    return tri_[i];
  }

  IndexRange tri_range() const
  {
    return IndexRange(tri_.size());
  }

  Span<int> tris() const
  {
    return Span<int>(tri_);
  }

  int cell_above{NO_INDEX};
  int cell_below{NO_INDEX};
};

static std::ostream &operator<<(std::ostream &os, const Patch &patch)
{
  os << "Patch " << patch.tri();
  if (patch.cell_above != NO_INDEX) {
    os << " cell_above=" << patch.cell_above;
  }
  else {
    os << " cell_above not set";
  }
  if (patch.cell_below != NO_INDEX) {
    os << " cell_below=" << patch.cell_below;
  }
  else {
    os << " cell_below not set";
  }
  return os;
}

class PatchesInfo {
  /* All of the Patches for a Mesh. */
  Vector<Patch> patch_;
  /* Patch index for corresponding triangle. */
  Array<int> tri_patch_;
  /* Shared edge for incident patches; (-1, -1) if none. */
  Map<std::pair<int, int>, Edge> pp_edge_;

 public:
  explicit PatchesInfo(int ntri)
  {
    constexpr int max_expected_patch_patch_incidences = 100;
    tri_patch_ = Array<int>(ntri, NO_INDEX);
    pp_edge_.reserve(max_expected_patch_patch_incidences);
  }

  int tri_patch(int t) const
  {
    return tri_patch_[t];
  }

  int add_patch()
  {
    int patch_index = patch_.append_and_get_index(Patch());
    return patch_index;
  }

  void grow_patch(int patch_index, int t)
  {
    tri_patch_[t] = patch_index;
    patch_[patch_index].add_tri(t);
  }

  bool tri_is_assigned(int t) const
  {
    return tri_patch_[t] != NO_INDEX;
  }

  const Patch &patch(int patch_index) const
  {
    return patch_[patch_index];
  }

  Patch &patch(int patch_index)
  {
    return patch_[patch_index];
  }

  int tot_patch() const
  {
    return patch_.size();
  }

  IndexRange index_range() const
  {
    return IndexRange(patch_.size());
  }

  const Patch *begin() const
  {
    return patch_.begin();
  }

  const Patch *end() const
  {
    return patch_.end();
  }

  void add_new_patch_patch_edge(int p1, int p2, Edge e)
  {
    pp_edge_.add_new(std::pair<int, int>(p1, p2), e);
    pp_edge_.add_new(std::pair<int, int>(p2, p1), e);
  }

  Edge patch_patch_edge(int p1, int p2)
  {
    return pp_edge_.lookup_default(std::pair<int, int>(p1, p2), Edge());
  }
};

static bool apply_bool_op(int bool_optype, const Array<int> &winding);

/* A Cell is a volume of 3-space, surrounded by patches.
 * We will partition all 3-space into Cells.
 * One cell, the Ambient cell, contains all other cells.
 */
class Cell {
  Vector<int> patches_;
  Array<int> winding_;
  bool winding_assigned_{false};
  bool flag_{false};

 public:
  Cell() = default;

  void add_patch(int p)
  {
    patches_.append(p);
  }

  const Vector<int> &patches() const
  {
    return patches_;
  }

  const Array<int> &winding() const
  {
    return winding_;
  }

  void init_winding(int winding_len)
  {
    winding_ = Array<int>(winding_len);
  }

  void seed_ambient_winding()
  {
    winding_.fill(0);
    winding_assigned_ = true;
  }

  void set_winding_and_flag(const Cell &from_cell, int shape, int delta, int bool_optype)
  {
    std::copy(from_cell.winding().begin(), from_cell.winding().end(), winding_.begin());
    winding_[shape] += delta;
    winding_assigned_ = true;
    flag_ = apply_bool_op(bool_optype, winding_);
  }

  bool flag() const
  {
    return flag_;
  }

  bool winding_assigned() const
  {
    return winding_assigned_;
  }
};

static std::ostream &operator<<(std::ostream &os, const Cell &cell)
{
  os << "Cell patches " << cell.patches();
  if (cell.winding().size() > 0) {
    os << " winding " << cell.winding();
    os << " flag " << cell.flag();
  }
  return os;
}

/* Information about all the Cells. */
class CellsInfo {
  Vector<Cell> cell_;

 public:
  CellsInfo() = default;

  int add_cell()
  {
    int index = cell_.append_and_get_index(Cell());
    return static_cast<int>(index);
  }

  Cell &cell(int c)
  {
    return cell_[c];
  }

  const Cell &cell(int c) const
  {
    return cell_[c];
  }

  int tot_cell() const
  {
    return cell_.size();
  }

  IndexRange index_range() const
  {
    return cell_.index_range();
  }

  const Cell *begin() const
  {
    return cell_.begin();
  }

  const Cell *end() const
  {
    return cell_.end();
  }

  void init_windings(int winding_len)
  {
    for (Cell &cell : cell_) {
      cell.init_winding(winding_len);
    }
  }
};

/* Partition the triangles of tm into Patches. */
static PatchesInfo find_patches(const Mesh &tm, const TriMeshTopology &tmtopo)
{
  const int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nFIND_PATCHES\n";
  }
  int ntri = tm.face_size();
  PatchesInfo pinfo(ntri);
  /* Algorithm: Grow patches across manifold edges as long as there are unassigned triangles. */
  Stack<int> cur_patch_grow;
  for (int t : tm.face_index_range()) {
    if (pinfo.tri_patch(t) == -1) {
      cur_patch_grow.push(t);
      int cur_patch_index = pinfo.add_patch();
      while (!cur_patch_grow.is_empty()) {
        int tcand = cur_patch_grow.pop();
        if (dbg_level > 1) {
          std::cout << "pop tcand = " << tcand << "; assigned = " << pinfo.tri_is_assigned(tcand)
                    << "\n";
        }
        if (pinfo.tri_is_assigned(tcand)) {
          continue;
        }
        if (dbg_level > 1) {
          std::cout << "grow patch from seed tcand=" << tcand << "\n";
        }
        pinfo.grow_patch(cur_patch_index, tcand);
        const Face &tri = *tm.face(tcand);
        for (int i = 0; i < 3; ++i) {
          Edge e(tri[i], tri[(i + 1) % 3]);
          int t_other = tmtopo.other_tri_if_manifold(e, tcand);
          if (dbg_level > 1) {
            std::cout << "  edge " << e << " generates t_other=" << t_other << "\n";
          }
          if (t_other != NO_INDEX) {
            if (!pinfo.tri_is_assigned(t_other)) {
              if (dbg_level > 1) {
                std::cout << "    push t_other = " << t_other << "\n";
              }
              cur_patch_grow.push(t_other);
            }
          }
          else {
            /* e is non-manifold. Set any patch-patch incidences we can. */
            if (dbg_level > 1) {
              std::cout << "    e non-manifold case\n";
            }
            const Vector<int> *etris = tmtopo.edge_tris(e);
            if (etris != nullptr) {
              for (int i : etris->index_range()) {
                int t_other = (*etris)[i];
                if (t_other != tcand && pinfo.tri_is_assigned(t_other)) {
                  int p_other = pinfo.tri_patch(t_other);
                  if (p_other == cur_patch_index) {
                    continue;
                  }
                  if (pinfo.patch_patch_edge(cur_patch_index, p_other).v0() == nullptr) {
                    pinfo.add_new_patch_patch_edge(cur_patch_index, p_other, e);
                    if (dbg_level > 1) {
                      std::cout << "added patch_patch_edge (" << cur_patch_index << "," << p_other
                                << ") = " << e << "\n";
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  if (dbg_level > 0) {
    std::cout << "\nafter FIND_PATCHES: found " << pinfo.tot_patch() << " patches\n";
    for (int p : pinfo.index_range()) {
      std::cout << p << ": " << pinfo.patch(p) << "\n";
    }
    if (dbg_level > 1) {
      std::cout << "\ntriangle map\n";
      for (int t : tm.face_index_range()) {
        std::cout << t << ": patch " << pinfo.tri_patch(t) << "\n";
      }
    }
    std::cout << "\npatch-patch incidences\n";
    for (int p1 : pinfo.index_range()) {
      for (int p2 : pinfo.index_range()) {
        Edge e = pinfo.patch_patch_edge(p1, p2);
        if (e.v0() != nullptr) {
          std::cout << "p" << p1 << " and p" << p2 << " share edge " << e << "\n";
        }
      }
    }
  }
  return pinfo;
}

/* If e is an edge in tri, return the vertex that isn't part of tri,
 * the "flap" vertex, or nullptr if e is not part of tri.
 * Also, e may be reversed in tri.
 * Set *r_rev to true if it is reversed, else false.
 */
static Vertp find_flap_vert(const Face &tri, const Edge e, bool *r_rev)
{
  *r_rev = false;
  Vertp flapv;
  if (tri[0] == e.v0()) {
    if (tri[1] == e.v1()) {
      *r_rev = false;
      flapv = tri[2];
    }
    else {
      if (tri[2] != e.v1()) {
        return nullptr;
      }
      *r_rev = true;
      flapv = tri[1];
    }
  }
  else if (tri[1] == e.v0()) {
    if (tri[2] == e.v1()) {
      *r_rev = false;
      flapv = tri[0];
    }
    else {
      if (tri[0] != e.v1()) {
        return nullptr;
      }
      *r_rev = true;
      flapv = tri[2];
    }
  }
  else {
    if (tri[2] != e.v0()) {
      return nullptr;
    }
    if (tri[0] == e.v1()) {
      *r_rev = false;
      flapv = tri[1];
    }
    else {
      if (tri[1] != e.v1()) {
        return nullptr;
      }
      *r_rev = true;
      flapv = tri[0];
    }
  }
  return flapv;
}

/*
 * Triangle tri and tri0 share edge e.
 * Classify tri with respect to tri0 as described in
 * sort_tris_around_edge, and return 1, 2, 3, or 4 as tri is:
 * (1) coplanar with tri0 and on same side of e
 * (2) coplanar with tri0 and on opposite side of e
 * (3) below plane of tri0
 * (4) above plane of tri0
 * For "above" and "below", we use the orientation of non-reversed
 * orientation of tri0.
 * Because of the way the intersect mesh was made, we can assume
 * that if a triangle is in class 1 then it is has the same flap vert
 * as tri0.
 */
static int sort_tris_class(const Face &tri, const Face &tri0, const Edge e)
{
  const int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "classify  e = " << e << "\n";
  }
  mpq3 a0 = tri0[0]->co_exact;
  mpq3 a1 = tri0[1]->co_exact;
  mpq3 a2 = tri0[2]->co_exact;
  bool rev;
  bool rev0;
  Vertp flapv0 = find_flap_vert(tri0, e, &rev0);
  Vertp flapv = find_flap_vert(tri, e, &rev);
  if (dbg_level > 0) {
    std::cout << " t0 = " << tri0[0] << " " << tri0[1] << " " << tri0[2];
    std::cout << " rev0 = " << rev0 << " flapv0 = " << flapv0 << "\n";
    std::cout << " t = " << tri[0] << " " << tri[1] << " " << tri[2];
    std::cout << " rev = " << rev << " flapv = " << flapv << "\n";
  }
  BLI_assert(flapv != nullptr && flapv0 != nullptr);
  const mpq3 flap = flapv->co_exact;
  /* orient will be positive if flap is below oriented plane of a0,a1,a2. */
  int orient = mpq3::orient3d(a0, a1, a2, flap);
  int ans;
  if (orient > 0) {
    ans = rev0 ? 4 : 3;
  }
  else if (orient < 0) {
    ans = rev0 ? 3 : 4;
  }
  else {
    ans = flapv == flapv0 ? 1 : 2;
  }
  if (dbg_level > 0) {
    std::cout << " orient = " << orient << " ans = " << ans << "\n";
  }
  return ans;
}

/* To ensure consistent ordering of coplanar triangles if they happen to be sorted around
 * more than one edge, sort the triangle indices in g (in place) by their index -- but also apply
 * a sign to the index: positive if the triangle has edge e in the same orientation,
 * otherwise negative.
 */
static void sort_by_signed_triangle_index(Vector<int> &g, const Edge e, const Mesh &tm)
{
  Array<int> signed_g(g.size());
  for (int i : g.index_range()) {
    const Face &tri = *tm.face(g[i]);
    bool rev;
    find_flap_vert(tri, e, &rev);
    signed_g[i] = rev ? -g[i] : g[i];
  }
  std::sort(signed_g.begin(), signed_g.end());

  for (int i : g.index_range()) {
    g[i] = abs(signed_g[i]);
  }
}

constexpr int EXTRA_TRI_INDEX = INT_MAX;

/*
 * Sort the triangles tris, which all share edge e, as they appear
 * geometrically clockwise when looking down edge e.
 * Triangle t0 is the first triangle in the toplevel call
 * to this recursive routine. The merge step below differs
 * for the top level call and all the rest, so this distinguishes those cases.
 * Care is taken in the case of duplicate triangles to have
 * an ordering that is consistent with that which would happen
 * if another edge of the triangle were sorted around.
 *
 * We sometimes need to do this with an extra triangle that is not part of tm.
 * To accommodate this:
 * If extra_tri is non-null, then an index of EXTRA_TRI_INDEX should use it for the triangle.
 */
static Array<int> sort_tris_around_edge(const Mesh &tm,
                                        const TriMeshTopology &tmtopo,
                                        const Edge e,
                                        const Span<int> &tris,
                                        const int t0,
                                        Facep extra_tri)
{
  /* Divide and conquer, quicksort-like sort.
   * Pick a triangle t0, then partition into groups:
   * (1) coplanar with t0 and on same side of e
   * (2) coplanar with t0 and on opposite side of e
   * (3) below plane of t0
   * (4) above plane of t0
   * Each group is sorted and then the sorts are merged to give the answer.
   * We don't expect the input array to be very large - should typically
   * be only 3 or 4 - so OK to make copies of arrays instead of swapping
   * around in a single array.
   */
  const int dbg_level = 0;
  if (tris.size() == 0) {
    return Array<int>();
  }
  if (dbg_level > 0) {
    if (t0 == tris[0]) {
      std::cout << "\n";
    }
    std::cout << "sort_tris_around_edge " << e << "\n";
    std::cout << "tris = " << tris << "\n";
  }
  Vector<int> g1{tris[0]};
  Vector<int> g2;
  Vector<int> g3;
  Vector<int> g4;
  Vector<int> *groups[] = {&g1, &g2, &g3, &g4};
  const Face &tri0 = *tm.face(t0);
  for (int i : tris.index_range()) {
    if (i == 0) {
      continue;
    }
    int t = tris[i];
    BLI_assert(t < tm.face_size() || (t == EXTRA_TRI_INDEX && extra_tri != nullptr));
    const Face &tri = (t == EXTRA_TRI_INDEX) ? *extra_tri : *tm.face(t);
    if (dbg_level > 2) {
      std::cout << "classifying tri " << t << " with respect to " << t0 << "\n";
    }
    int group_num = sort_tris_class(tri, tri0, e);
    if (dbg_level > 2) {
      std::cout << "  classify result : " << group_num << "\n";
    }
    groups[group_num - 1]->append(t);
  }
  if (dbg_level > 1) {
    std::cout << "g1 = " << g1 << "\n";
    std::cout << "g2 = " << g2 << "\n";
    std::cout << "g3 = " << g3 << "\n";
    std::cout << "g4 = " << g4 << "\n";
  }
  if (g1.size() > 1) {
    sort_by_signed_triangle_index(g1, e, tm);
    if (dbg_level > 1) {
      std::cout << "g1 sorted: " << g1 << "\n";
    }
  }
  if (g2.size() > 1) {
    sort_by_signed_triangle_index(g2, e, tm);
    if (dbg_level > 1) {
      std::cout << "g2 sorted: " << g2 << "\n";
    }
  }
  if (g3.size() > 1) {
    Array<int> g3sorted = sort_tris_around_edge(tm, tmtopo, e, g3, g3[0], extra_tri);
    std::copy(g3sorted.begin(), g3sorted.end(), g3.begin());
    if (dbg_level > 1) {
      std::cout << "g3 sorted: " << g3 << "\n";
    }
  }
  if (g4.size() > 1) {
    Array<int> g4sorted = sort_tris_around_edge(tm, tmtopo, e, g4, g4[0], extra_tri);
    std::copy(g4sorted.begin(), g4sorted.end(), g4.begin());
    if (dbg_level > 1) {
      std::cout << "g4 sorted: " << g4 << "\n";
    }
  }
  int group_tot_size = g1.size() + g2.size() + g3.size() + g4.size();
  Array<int> ans(group_tot_size);
  int *p = ans.begin();
  if (tris[0] == t0) {
    p = std::copy(g1.begin(), g1.end(), p);
    p = std::copy(g4.begin(), g4.end(), p);
    p = std::copy(g2.begin(), g2.end(), p);
    std::copy(g3.begin(), g3.end(), p);
  }
  else {
    p = std::copy(g3.begin(), g3.end(), p);
    p = std::copy(g1.begin(), g1.end(), p);
    p = std::copy(g4.begin(), g4.end(), p);
    std::copy(g2.begin(), g2.end(), p);
  }
  if (dbg_level > 0) {
    std::cout << "sorted tris = " << ans << "\n";
  }
  return ans;
}

/* Find the Cells around edge e.
 * This possibly makes new cells in cinfo, and sets up the
 * bipartite graph edges between cells and patches.
 * Will modify pinfo and cinfo and the patches and cells they contain.
 */
static void find_cells_from_edge(const Mesh &tm,
                                 const TriMeshTopology &tmtopo,
                                 PatchesInfo &pinfo,
                                 CellsInfo &cinfo,
                                 const Edge e)
{
  const int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "find_cells_from_edge " << e << "\n";
  }
  const Vector<int> *edge_tris = tmtopo.edge_tris(e);
  BLI_assert(edge_tris != nullptr);
  Array<int> sorted_tris = sort_tris_around_edge(
      tm, tmtopo, e, Span<int>(*edge_tris), (*edge_tris)[0], nullptr);

  int n_edge_tris = static_cast<int>(edge_tris->size());
  Array<int> edge_patches(n_edge_tris);
  for (int i = 0; i < n_edge_tris; ++i) {
    edge_patches[i] = pinfo.tri_patch(sorted_tris[i]);
    if (dbg_level > 1) {
      std::cout << "edge_patches[" << i << "] = " << edge_patches[i] << "\n";
    }
  }
  for (int i = 0; i < n_edge_tris; ++i) {
    int inext = (i + 1) % n_edge_tris;
    int r_index = edge_patches[i];
    int rnext_index = edge_patches[inext];
    Patch &r = pinfo.patch(r_index);
    Patch &rnext = pinfo.patch(rnext_index);
    bool r_flipped;
    bool rnext_flipped;
    find_flap_vert(*tm.face(sorted_tris[i]), e, &r_flipped);
    find_flap_vert(*tm.face(sorted_tris[inext]), e, &rnext_flipped);
    int *r_follow_cell = r_flipped ? &r.cell_below : &r.cell_above;
    int *rnext_prev_cell = rnext_flipped ? &rnext.cell_above : &rnext.cell_below;
    if (dbg_level > 0) {
      std::cout << "process patch pair " << r_index << " " << rnext_index << "\n";
      std::cout << "  r_flipped = " << r_flipped << " rnext_flipped = " << rnext_flipped << "\n";
      std::cout << "  r_follow_cell (" << (r_flipped ? "below" : "above")
                << ") = " << *r_follow_cell << "\n";
      std::cout << "  rnext_prev_cell (" << (rnext_flipped ? "above" : "below")
                << ") = " << *rnext_prev_cell << "\n";
    }
    if (*r_follow_cell == NO_INDEX && *rnext_prev_cell == NO_INDEX) {
      /* Neither is assigned: make a new cell. */
      int c = cinfo.add_cell();
      *r_follow_cell = c;
      *rnext_prev_cell = c;
      Cell &cell = cinfo.cell(c);
      cell.add_patch(r_index);
      cell.add_patch(rnext_index);
      if (dbg_level > 0) {
        std::cout << "  made new cell " << c << "\n";
        std::cout << "  p" << r_index << "." << (r_flipped ? "cell_below" : "cell_above") << " = c"
                  << c << "\n";
        std::cout << "  p" << rnext_index << "." << (rnext_flipped ? "cell_above" : "cell_below")
                  << " = c" << c << "\n";
      }
    }
    else if (*r_follow_cell != NO_INDEX && *rnext_prev_cell == NO_INDEX) {
      int c = *r_follow_cell;
      *rnext_prev_cell = c;
      cinfo.cell(c).add_patch(rnext_index);
      if (dbg_level > 0) {
        std::cout << "  p" << r_index << "." << (r_flipped ? "cell_below" : "cell_above") << " = c"
                  << c << "\n";
      }
    }
    else if (*r_follow_cell == NO_INDEX && *rnext_prev_cell != NO_INDEX) {
      int c = *rnext_prev_cell;
      *r_follow_cell = c;
      cinfo.cell(c).add_patch(r_index);
      if (dbg_level > 0) {
        std::cout << "  p" << rnext_index << "." << (rnext_flipped ? "cell_above" : "cell_below")
                  << " = c" << c << "\n";
      }
    }
    else {
      if (*r_follow_cell != *rnext_prev_cell) {
        std::cout << "IMPLEMENT ME: MERGE CELLS\n";
        BLI_assert(false);
      }
    }
  }
}

/* Find the partition of 3-space into Cells.
 * This assigns the cell_above and cell_below for each Patch.
 */
static CellsInfo find_cells(const Mesh &tm, const TriMeshTopology &tmtopo, PatchesInfo &pinfo)
{
  const int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nFIND_CELLS\n";
  }
  CellsInfo cinfo;
  /* For each unique edge shared between patch pairs, process it. */
  Set<Edge> processed_edges;
  int np = pinfo.tot_patch();
  for (int p = 0; p < np; ++p) {
    for (int q = p + 1; q < np; ++q) {
      Edge e = pinfo.patch_patch_edge(p, q);
      if (e.v0() != nullptr) {
        if (!processed_edges.contains(e)) {
          processed_edges.add_new(e);
          find_cells_from_edge(tm, tmtopo, pinfo, cinfo, e);
        }
      }
    }
  }
  if (dbg_level > 0) {
    std::cout << "\nFIND_CELLS found " << cinfo.tot_cell() << " cells\nCells\n";
    for (int i : cinfo.index_range()) {
      std::cout << i << ": " << cinfo.cell(i) << "\n";
    }
    std::cout << "Patches\n";
    for (int i : pinfo.index_range()) {
      std::cout << i << ": " << pinfo.patch(i) << "\n";
    }
  }
  return cinfo;
}

static bool patch_cell_graph_connected(const CellsInfo &cinfo, const PatchesInfo &pinfo)
{
  if (cinfo.tot_cell() == 0 || pinfo.tot_patch() == 0) {
    return false;
  }
  Array<bool> cell_reachable(cinfo.tot_cell(), false);
  Array<bool> patch_reachable(pinfo.tot_patch(), false);
  Stack<int> stack; /* Patch indexes to visit. */
  stack.push(0);
  while (!stack.is_empty()) {
    int p = stack.pop();
    if (patch_reachable[p]) {
      continue;
    }
    patch_reachable[p] = true;
    const Patch &patch = pinfo.patch(p);
    for (int c : {patch.cell_above, patch.cell_below}) {
      if (cell_reachable[c]) {
        continue;
      }
      cell_reachable[c] = true;
      for (int p : cinfo.cell(c).patches()) {
        if (!patch_reachable[p]) {
          stack.push(p);
        }
      }
    }
  }
  if (std::any_of(cell_reachable.begin(), cell_reachable.end(), std::logical_not<>())) {
    return false;
  }
  if (std::any_of(patch_reachable.begin(), patch_reachable.end(), std::logical_not<>())) {
    return false;
  }
  return true;
}

/* Do all patches have cell_above and cell_below set?
 * Is the bipartite graph connected?
 */
static bool patch_cell_graph_ok(const CellsInfo &cinfo, const PatchesInfo &pinfo)
{
  for (int c : cinfo.index_range()) {
    const Cell &cell = cinfo.cell(c);
    if (cell.patches().size() == 0) {
      std::cout << "Patch/Cell graph disconnected at Cell " << c << " with no patches\n";
      return false;
    }
    for (int p : cell.patches()) {
      if (p >= pinfo.tot_patch()) {
        std::cout << "Patch/Cell graph has bad patch index at Cell " << c << "\n";
        return false;
      }
    }
  }
  for (int p : pinfo.index_range()) {
    const Patch &patch = pinfo.patch(p);
    if (patch.cell_above == NO_INDEX || patch.cell_below == NO_INDEX) {
      std::cout << "Patch/Cell graph disconnected at Patch " << p
                << " with one or two missing cells\n";
      return false;
    }
    if (patch.cell_above >= cinfo.tot_cell() || patch.cell_below >= cinfo.tot_cell()) {
      std::cout << "Patch/Cell graph has bad cell index at Patch " << p << "\n";
      return false;
    }
  }
  if (!patch_cell_graph_connected(cinfo, pinfo)) {
    std::cout << "Patch/Cell graph not connected\n";
    return false;
  }
  return true;
}

/*
 * Find the ambient cell -- that is, the cell that is outside
 * all other cells.
 */
static int find_ambient_cell(const Mesh &tm,
                             const TriMeshTopology &tmtopo,
                             const PatchesInfo pinfo,
                             MArena *arena)
{
  int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "FIND_AMBIENT_CELL\n";
  }
  /* First find a vertex with the maximum x value. */
  /* Prefer not to populate the verts in the Mesh just for this. */
  Vertp v_extreme = (*tm.face(0))[0];
  mpq_class extreme_x = v_extreme->co_exact.x;
  for (Facep f : tm.faces()) {
    for (Vertp v : *f) {
      const mpq_class &x = v->co_exact.x;
      if (x > extreme_x) {
        v_extreme = v;
        extreme_x = x;
      }
    }
  }
  if (dbg_level > 0) {
    std::cout << "v_extreme = " << v_extreme << "\n";
  }
  /* Find edge attached to v_extreme with max absolute slope
   * when projected onto the xy plane. That edge is guaranteed to
   * be on the convex hull of the mesh.
   */
  const Vector<Edge> &edges = tmtopo.vert_edges(v_extreme);
  const mpq_class extreme_y = v_extreme->co_exact.y;
  Edge ehull;
  mpq_class max_abs_slope = -1;
  for (Edge e : edges) {
    const Vertp v_other = (e.v0() == v_extreme) ? e.v1() : e.v0();
    const mpq3 &co_other = v_other->co_exact;
    mpq_class delta_x = co_other.x - extreme_x;
    if (delta_x == 0) {
      /* Vertical slope. */
      ehull = e;
      break;
    }
    mpq_class abs_slope = abs((co_other.y - extreme_y) / delta_x);
    if (abs_slope > max_abs_slope) {
      ehull = e;
      max_abs_slope = abs_slope;
    }
  }
  if (dbg_level > 0) {
    std::cout << "ehull = " << ehull << " slope = " << max_abs_slope << "\n";
  }
  /* Sort triangles around ehull, including a dummy triangle that include a known point in ambient
   * cell. */
  mpq3 p_in_ambient = v_extreme->co_exact;
  p_in_ambient.x += 1;
  const Vector<int> *ehull_edge_tris = tmtopo.edge_tris(ehull);
  Vertp dummy_vert = arena->add_or_find_vert(p_in_ambient, NO_INDEX);
  Facep dummy_tri = arena->add_face(
      {ehull.v0(), ehull.v1(), dummy_vert}, NO_INDEX, {NO_INDEX, NO_INDEX, NO_INDEX});
  Array<int> edge_tris(ehull_edge_tris->size() + 1);
  std::copy(ehull_edge_tris->begin(), ehull_edge_tris->end(), edge_tris.begin());
  edge_tris[edge_tris.size() - 1] = EXTRA_TRI_INDEX;
  Array<int> sorted_tris = sort_tris_around_edge(
      tm, tmtopo, ehull, edge_tris, edge_tris[0], dummy_tri);
  if (dbg_level > 0) {
    std::cout << "sorted tris = " << sorted_tris << "\n";
  }
  int *p_sorted_dummy = std::find(sorted_tris.begin(), sorted_tris.end(), EXTRA_TRI_INDEX);
  BLI_assert(p_sorted_dummy != sorted_tris.end());
  int dummy_index = p_sorted_dummy - sorted_tris.begin();
  int prev_tri = (dummy_index == 0) ? sorted_tris[sorted_tris.size() - 1] :
                                      sorted_tris[dummy_index - 1];
  int next_tri = (dummy_index == static_cast<int>(sorted_tris.size() - 1)) ?
                     sorted_tris[0] :
                     sorted_tris[dummy_index + 1];
  if (dbg_level > 0) {
    std::cout << "prev tri to dummy = " << prev_tri << ";  next tri to dummy = " << next_tri
              << "\n";
  }
  const Patch &prev_patch = pinfo.patch(pinfo.tri_patch(prev_tri));
  const Patch &next_patch = pinfo.patch(pinfo.tri_patch(next_tri));
  if (dbg_level > 0) {
    std::cout << "prev_patch = " << prev_patch << ", next_patch = " << next_patch << "\n";
  }
  BLI_assert(prev_patch.cell_above == next_patch.cell_above);
  if (dbg_level > 0) {
    std::cout << "FIND_AMBIENT_CELL returns " << prev_patch.cell_above << "\n";
  }
  return prev_patch.cell_above;
}

/* Starting with ambient cell c_ambient, with all zeros for winding numbers,
 * propagate winding numbers to all the other cells.
 * There will be a vector of nshapes winding numbers in each cell, one per
 * input shape.
 * As one crosses a patch into a new cell, the original shape (mesh part)
 * that that patch was part of dictates which winding number changes.
 * The shape_fn(triangle_number) function should return the shape that the
 * triangle is part of.
 * Also, as soon as the winding numbers for a cell are set, use bool_optype
 * to decide whether that cell is included or excluded from the boolean output.
 * If included, the cell's flag will be set to true.
 */
static void propagate_windings_and_flag(PatchesInfo &pinfo,
                                        CellsInfo &cinfo,
                                        int c_ambient,
                                        bool_optype op,
                                        int nshapes,
                                        std::function<int(int)> shape_fn)
{
  int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "PROPAGATE_WINDINGS, ambient cell = " << c_ambient << "\n";
  }
  Cell &cell_ambient = cinfo.cell(c_ambient);
  cell_ambient.seed_ambient_winding();
  /* Use a vector as a queue. It can't grow bigger than number of cells. */
  Vector<int> queue;
  queue.reserve(cinfo.tot_cell());
  int queue_head = 0;
  queue.append(c_ambient);
  while (queue_head < queue.size()) {
    int c = queue[queue_head++];
    if (dbg_level > 1) {
      std::cout << "process cell " << c << "\n";
    }
    Cell &cell = cinfo.cell(c);
    for (int p : cell.patches()) {
      Patch &patch = pinfo.patch(p);
      bool p_above_c = patch.cell_below == c;
      int c_neighbor = p_above_c ? patch.cell_above : patch.cell_below;
      if (dbg_level > 1) {
        std::cout << "  patch " << p << " p_above_c = " << p_above_c << "\n";
        std::cout << "    c_neighbor = " << c_neighbor << "\n";
      }
      Cell &cell_neighbor = cinfo.cell(c_neighbor);
      if (!cell_neighbor.winding_assigned()) {
        int winding_delta = p_above_c ? -1 : 1;
        int t = patch.tri(0);
        int shape = shape_fn(t);
        BLI_assert(shape < nshapes);
        if (dbg_level > 1) {
          std::cout << "    representative tri " << t << ": in shape " << shape << "\n";
        }
        cell_neighbor.set_winding_and_flag(cell, shape, winding_delta, op);
        if (dbg_level > 1) {
          std::cout << "    now cell_neighbor = " << cell_neighbor << "\n";
        }
        queue.append(c_neighbor);
        BLI_assert(queue.size() <= cinfo.tot_cell());
      }
    }
  }
  if (dbg_level > 0) {
    std::cout << "\nPROPAGATE_WINDINGS result\n";
    for (int i = 0; i < cinfo.tot_cell(); ++i) {
      std::cout << i << ": " << cinfo.cell(i) << "\n";
    }
  }
}

/* Given an array of winding numbers, where the ith entry is a cell's winding
 * number with respect to input shape (mesh part) i, return true if the
 * cell should be included in the output of the boolean operation.
 *   Intersection: all the winding numbers must be nonzero.
 *   Union: at least one winding number must be nonzero.
 *   Difference (first shape minus the rest): first winding number must be nonzero
 *      and the rest must have at least one zero winding number.
 */
static bool apply_bool_op(int bool_optype, const Array<int> &winding)
{
  int nw = static_cast<int>(winding.size());
  BLI_assert(nw > 0);
  switch (bool_optype) {
    case BOOLEAN_ISECT: {
      for (int i = 0; i < nw; ++i) {
        if (winding[i] == 0) {
          return false;
        }
      }
      return true;
    } break;
    case BOOLEAN_UNION: {
      for (int i = 0; i < nw; ++i) {
        if (winding[i] != 0) {
          return true;
        }
      }
      return false;
    } break;
    case BOOLEAN_DIFFERENCE: {
      /* if nw > 2, make it shape 0 minus the union of the rest. */
      if (winding[0] == 0) {
        return false;
      }
      if (nw == 1) {
        return true;
      }
      for (int i = 1; i < nw; ++i) {
        if (winding[i] == 0) {
          return true;
        }
      }
      return false;
    } break;
    default:
      return false;
  }
}

/* Extract the output mesh from tm_subdivided and return it as a new mesh.
 * The cells in cinfo must have cells-to-be-retained flagged.
 * We keep only triangles between flagged and unflagged cells.
 * We flip the normals of any triangle that has a flagged cell above
 * and an unflagged cell below.
 * For all stacks of exact duplicate coplanar triangles, add up orientations
 * as +1 or -1 for each according to CCW vs CW. If the result is nonzero,
 * keep one copy with orientation chosen according to the dominant sign.
 */
static Mesh extract_from_flag_diffs(const Mesh &tm_subdivided,
                                    const PatchesInfo &pinfo,
                                    const CellsInfo &cinfo,
                                    MArena *arena)
{
  const int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nEXTRACT_FROM_FLAG_DIFFS\n";
  }
  Vector<Facep> out_tris;
  out_tris.reserve(tm_subdivided.face_size());
  for (int t : tm_subdivided.face_index_range()) {
    int p = pinfo.tri_patch(t);
    const Patch &patch = pinfo.patch(p);
    const Cell &cell_above = cinfo.cell(patch.cell_above);
    const Cell &cell_below = cinfo.cell(patch.cell_below);
    if (dbg_level > 0) {
      std::cout << "tri " << t << ": cell_above=" << patch.cell_above
                << " cell_below=" << patch.cell_below << "\n";
      std::cout << " flag_above=" << cell_above.flag() << " flag_below=" << cell_below.flag()
                << "\n";
    }
    if (cell_above.flag() ^ cell_below.flag()) {
      if (dbg_level > 0) {
        std::cout << "need tri " << t << "\n";
      }
      bool flip = cell_above.flag();
      Facep f = tm_subdivided.face(t);
      if (flip) {
        const Face &tri = *f;
        Array<Vertp> flipped_vs = {tri[0], tri[2], tri[1]};
        Array<int> flipped_e_origs = {tri.edge_orig[2], tri.edge_orig[1], tri.edge_orig[0]};
        Facep flipped_f = arena->add_face(flipped_vs, f->orig, flipped_e_origs);
        out_tris.append(flipped_f);
      }
      else {
        out_tris.append(f);
      }
    }
  }
  return Mesh(out_tris);
}

static const char *bool_optype_name(bool_optype op)
{
  switch (op) {
    case BOOLEAN_NONE:
      return "none";
      break;
    case BOOLEAN_ISECT:
      return "intersect";
      break;
    case BOOLEAN_UNION:
      return "union";
      break;
    case BOOLEAN_DIFFERENCE:
      return "difference";
    default:
      return "<unknown>";
  }
}

/* Which CDT output edge index is for an edge between output verts
 * v1 and v2 (in either order)? Return -1 if none.
 */
static int find_cdt_edge(const CDT_result<mpq_class> &cdt_out, int v1, int v2)
{
  for (int e : cdt_out.edge.index_range()) {
    const std::pair<int, int> &edge = cdt_out.edge[e];
    if ((edge.first == v1 && edge.second == v2) || (edge.first == v2 && edge.second == v1)) {
      return e;
    }
  }
  return -1;
}

/* Tesselate face f into triangles and return an array of Facep
 * giving that triangulation.
 * Care is taken so that the original edge index associated with
 * each edge in the output triangles either matches the original edge
 * for the (identical) edge of f, or else is -1. So diagonals added
 * for triangulation can later be indentified by having NO_INDEX for original.
 */
static Array<Facep> triangulate_poly(Facep f, MArena *arena)
{
  int flen = f->size();
  CDT_input<mpq_class> cdt_in;
  cdt_in.vert = Array<mpq2>(flen);
  cdt_in.face = Array<Vector<int>>(1);
  cdt_in.face[0].reserve(flen);
  for (int i : f->index_range()) {
    cdt_in.face[0].append(static_cast<int>(i));
  }
  /* Project poly along dominant axis of normal to get 2d coords. */
  const mpq3 &poly_normal = f->plane.norm_exact;
  int axis = mpq3::dominant_axis(poly_normal);
  /* If project down y axis as opposed to x or z, the orientation
   * of the polygon will be reversed.
   */
  int iflen = static_cast<int>(flen);
  bool rev = (axis == 1);
  for (int i = 0; i < iflen; ++i) {
    int ii = rev ? iflen - i - 1 : i;
    mpq2 &p2d = cdt_in.vert[ii];
    int k = 0;
    for (int j = 0; j < 3; ++j) {
      if (j != axis) {
        p2d[k++] = (*f)[ii]->co_exact[j];
      }
    }
  }
  CDT_result<mpq_class> cdt_out = delaunay_2d_calc(cdt_in, CDT_INSIDE);
  int n_tris = cdt_out.face.size();
  Array<Facep> ans(n_tris);
  for (int t = 0; t < n_tris; ++t) {
    int i_v_out[3];
    Vertp v[3];
    int eo[3];
    for (int i = 0; i < 3; ++i) {
      i_v_out[i] = cdt_out.face[t][i];
      v[i] = (*f)[cdt_out.vert_orig[i_v_out[i]][0]];
    }
    for (int i = 0; i < 3; ++i) {
      int e_out = find_cdt_edge(cdt_out, i_v_out[i], i_v_out[(i + 1) % 3]);
      BLI_assert(e_out != -1);
      eo[i] = NO_INDEX;
      for (int orig : cdt_out.edge_orig[e_out]) {
        if (orig != NO_INDEX) {
          eo[i] = orig;
          break;
        }
      }
    }
    ans[t] = arena->add_face({v[0], v[1], v[2]}, f->orig, {eo[0], eo[1], eo[2]});
  }
  return ans;
}

/* Return a Mesh that is a triangulation of a mesh with general
 * polygonal faces, pm.
 * Added diagonals will be distinguishable by having edge original
 * indices of NO_INDEX.
 */
static Mesh triangulate_polymesh(Mesh &pm, MArena *arena)
{
  Vector<Facep> face_tris;
  constexpr int estimated_tris_per_face = 3;
  face_tris.reserve(estimated_tris_per_face * pm.face_size());
  for (Facep f : pm.faces()) {
    /* Tesselate face f, following plan similar to BM_face_calc_tesselation. */
    int flen = f->size();
    if (flen == 3) {
      face_tris.append(f);
    }
    else if (flen == 4) {
      Vertp v0 = (*f)[0];
      Vertp v1 = (*f)[1];
      Vertp v2 = (*f)[2];
      Vertp v3 = (*f)[3];
      int eo_01 = f->edge_orig[0];
      int eo_12 = f->edge_orig[1];
      int eo_23 = f->edge_orig[2];
      int eo_30 = f->edge_orig[3];
      Facep f0 = arena->add_face({v0, v1, v2}, f->orig, {eo_01, eo_12, -1});
      Facep f1 = arena->add_face({v0, v2, v3}, f->orig, {-1, eo_23, eo_30});
      face_tris.append(f0);
      face_tris.append(f1);
    }
    else {
      Array<Facep> tris = triangulate_poly(f, arena);
      for (Facep tri : tris) {
        face_tris.append(tri);
      }
    }
  }
  return Mesh(face_tris);
}

/* If tri1 and tri2 have a common edge (in opposite orientation), return the indices into tri1 and
 * tri2 where that common edge starts. Else return (-1,-1).
 */
static std::pair<int, int> find_tris_common_edge(const Face &tri1, const Face &tri2)
{
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      if (tri1[(i + 1) % 3] == tri2[j] && tri1[i] == tri2[(j + 1) % 3]) {
        return std::pair<int, int>(i, j);
      }
    }
  }
  return std::pair<int, int>(-1, -1);
}

struct MergeEdge {
  /* Length (squared) of the edge, used for sorting. */
  double len_squared = 0.0;
  /* v1 and v2 are the ends of the edge, ordered so that v1->id < v2->id */
  Vertp v1 = nullptr;
  Vertp v2 = nullptr;
  /* left_face and right_face are indices into FaceMergeState->face. */
  int left_face = -1;
  int right_face = -1;
  int orig = -1; /* An edge orig index that can be used for this edge. */
  /* Is it allowed to dissolve this edge? */
  bool dissolvable = false;

  MergeEdge() = default;

  MergeEdge(Vertp va, Vertp vb)
  {
    if (va->id < vb->id) {
      this->v1 = va;
      this->v2 = vb;
    }
    else {
      this->v1 = vb;
      this->v2 = va;
    }
  };
};

struct MergeFace {
  /* The current sequence of Verts forming this face. */
  Vector<Vertp> vert;
  /* For each position in face, what is index in FaceMergeState of edge for that position? */
  Vector<int> edge;
  /* If not -1, merge_to gives a face index in FaceMergeState that this is merged to. */
  int merge_to = -1;
  /* A face->orig that can be used for the merged face. */
  int orig = -1;
};
struct FaceMergeState {
  /* The faces being considered for merging. Some will already have been merge (merge_to != -1). */
  Vector<MergeFace> face;
  /* The edges that are part of the faces in face[], together with current topological
   * information (their left and right faces) and whether or not they are dissolvable.
   */
  Vector<MergeEdge> edge;
  /* edge_map maps a pair of Vertp ids (in canonical order: smaller id first)
   * to the index in the above edge vector in which to find the corresonding MergeEdge.
   */
  Map<std::pair<int, int>, int> edge_map;
};

static std::ostream &operator<<(std::ostream &os, const FaceMergeState &fms)
{
  os << "faces:\n";
  for (int f : fms.face.index_range()) {
    const MergeFace &mf = fms.face[f];
    std::cout << f << ": orig=" << mf.orig << " verts ";
    for (Vertp v : mf.vert) {
      std::cout << v << " ";
    }
    std::cout << "\n";
    std::cout << "    edges " << mf.edge << "\n";
    std::cout << "    merge_to = " << mf.merge_to << "\n";
  }
  os << "\nedges:\n";
  for (int e : fms.edge.index_range()) {
    const MergeEdge &me = fms.edge[e];
    std::cout << e << ": (" << me.v1 << "," << me.v2 << ") left=" << me.left_face
              << " right=" << me.right_face << " dis=" << me.dissolvable << " orig=" << me.orig
              << "\n";
  }
  return os;
}

static void init_face_merge_state(FaceMergeState *fms, const Vector<int> &tris, const Mesh &tm)
{
  const int dbg_level = 0;
  /* Reserve enough faces and edges so that neither will have to resize. */
  fms->face.reserve(tris.size() + 1);
  fms->edge.reserve((3 * tris.size()));
  fms->edge_map.reserve(3 * tris.size());
  if (dbg_level > 0) {
    std::cout << "\nINIT_FACE_MERGE_STATE\n";
  }
  for (int t : tris.index_range()) {
    MergeFace mf;
    const Face &tri = *tm.face(tris[t]);
    mf.vert.append(tri[0]);
    mf.vert.append(tri[1]);
    mf.vert.append(tri[2]);
    int f = static_cast<int>(fms->face.append_and_get_index(mf));
    for (int i = 0; i < 3; ++i) {
      int inext = (i + 1) % 3;
      MergeEdge new_me(mf.vert[i], mf.vert[inext]);
      std::pair<int, int> canon_vs(new_me.v1->id, new_me.v2->id);
      int me_index = fms->edge_map.lookup_default(canon_vs, -1);
      if (me_index == -1) {
        double3 vec = new_me.v2->co - new_me.v1->co;
        new_me.len_squared = vec.length_squared();
        new_me.orig = tri.edge_orig[i];
        new_me.dissolvable = (new_me.orig == NO_INDEX);
        fms->edge.append(new_me);
        me_index = static_cast<int>(fms->edge.size()) - 1;
        fms->edge_map.add_new(canon_vs, me_index);
      }
      MergeEdge &me = fms->edge[me_index];
      if (me.dissolvable && tri.edge_orig[i] != NO_INDEX) {
        me.dissolvable = false;
        me.orig = tri.edge_orig[i];
      }
      /* This face is left or right depending on orientation of edge. */
      if (me.v1 == mf.vert[i]) {
        BLI_assert(me.left_face == -1);
        fms->edge[me_index].left_face = f;
      }
      else {
        BLI_assert(me.right_face == -1);
        fms->edge[me_index].right_face = f;
      }
      fms->face[f].edge.append(me_index);
    }
  }
  if (dbg_level > 0) {
    std::cout << *fms;
  }
}

/* To have a valid bmesh, there are constraints on what edges can be removed.
 * We cannot remove an edge if (a) it would create two disconnected boundary parts
 * (which will happen if there's another edge sharing the same two faces);
 * or (b) it would create a face with a repeated vertex.
 */
static bool dissolve_leaves_valid_bmesh(FaceMergeState *fms,
                                        const MergeEdge &me,
                                        int me_index,
                                        const MergeFace &mf_left,
                                        const MergeFace &mf_right)
{
  int a_edge_start = mf_left.edge.first_index_of_try(me_index);
  int b_edge_start = mf_right.edge.first_index_of_try(me_index);
  BLI_assert(a_edge_start != -1 && b_edge_start != -1);
  int alen = static_cast<int>(mf_left.vert.size());
  int blen = static_cast<int>(mf_right.vert.size());
  int b_left_face = me.right_face;
  bool ok = true;
  /* Is there another edge, not me, in A's face, whose right face is B's left? */
  for (int a_e_index = (a_edge_start + 1) % alen; ok && a_e_index != a_edge_start;
       a_e_index = (a_e_index + 1) % alen) {
    const MergeEdge &a_me_cur = fms->edge[mf_left.edge[a_e_index]];
    if (a_me_cur.right_face == b_left_face) {
      ok = false;
    }
  }
  /* Is there a vert in A, not me.v1 or me.v2, that is also in B?
   * One could avoid this O(n^2) algorithm if had a structure saying which faces a vertex touches.
   */
  for (int a_v_index = 0; ok && a_v_index < alen; ++a_v_index) {
    Vertp a_v = mf_left.vert[a_v_index];
    if (a_v != me.v1 && a_v != me.v2) {
      for (int b_v_index = 0; b_v_index < blen; ++b_v_index) {
        Vertp b_v = mf_right.vert[b_v_index];
        if (a_v == b_v) {
          ok = false;
        }
      }
    }
  }
  return ok;
}

/* mf_left and mf_right should share a MergeEdge me, having index me_index.
 * We change mf_left to remove edge me and insert the appropriate edges of
 * mf_right in between the start and end vertices of that edge.
 * We change the left face of the spliced-in edges to be mf_left's index.
 * We mark the merge_to property of mf_right, which is now in essence deleted.
 */
static void splice_faces(
    FaceMergeState *fms, MergeEdge &me, int me_index, MergeFace &mf_left, MergeFace &mf_right)
{
  int a_edge_start = mf_left.edge.first_index_of_try(me_index);
  int b_edge_start = mf_right.edge.first_index_of_try(me_index);
  BLI_assert(a_edge_start != -1 && b_edge_start != -1);
  int alen = static_cast<int>(mf_left.vert.size());
  int blen = static_cast<int>(mf_right.vert.size());
  Vector<Vertp> splice_vert;
  Vector<int> splice_edge;
  splice_vert.reserve(alen + blen - 2);
  splice_edge.reserve(alen + blen - 2);
  int ai = 0;
  while (ai < a_edge_start) {
    splice_vert.append(mf_left.vert[ai]);
    splice_edge.append(mf_left.edge[ai]);
    ++ai;
  }
  int bi = b_edge_start + 1;
  while (bi != b_edge_start) {
    if (bi >= blen) {
      bi = 0;
      if (bi == b_edge_start) {
        break;
      }
    }
    splice_vert.append(mf_right.vert[bi]);
    splice_edge.append(mf_right.edge[bi]);
    if (mf_right.vert[bi] == fms->edge[mf_right.edge[bi]].v1) {
      fms->edge[mf_right.edge[bi]].left_face = me.left_face;
    }
    else {
      fms->edge[mf_right.edge[bi]].right_face = me.left_face;
    }
    ++bi;
  }
  ai = a_edge_start + 1;
  while (ai < alen) {
    splice_vert.append(mf_left.vert[ai]);
    splice_edge.append(mf_left.edge[ai]);
    ++ai;
  }
  mf_right.merge_to = me.left_face;
  mf_left.vert = splice_vert;
  mf_left.edge = splice_edge;
  me.left_face = -1;
  me.right_face = -1;
}

/* Given that fms has been properly initialized to contain a set of faces that
 * together form a face or part of a face of the original Mesh, and that
 * it has properly recorded with faces are dissolvable, dissolve as many edges as possible.
 * We try to dissolve in decreasing order of edge length, so that it is more likely
 * that the final output doesn't have awkward looking long edges with extreme angles.
 */
static void do_dissolve(FaceMergeState *fms)
{
  const int dbg_level = 0;
  if (dbg_level > 1) {
    std::cout << "\nDO_DISSOLVE\n";
  }
  Vector<int> dissolve_edges;
  for (int e : fms->edge.index_range()) {
    if (fms->edge[e].dissolvable) {
      dissolve_edges.append(e);
    }
  }
  if (dissolve_edges.size() == 0) {
    return;
  }
  /* Things look nicer if we dissolve the longer edges first. */
  std::sort(
      dissolve_edges.begin(), dissolve_edges.end(), [fms](const int &a, const int &b) -> bool {
        return (fms->edge[a].len_squared > fms->edge[b].len_squared);
      });
  if (dbg_level > 0) {
    std::cout << "Sorted dissolvable edges: " << dissolve_edges << "\n";
  }
  for (int me_index : dissolve_edges) {
    MergeEdge &me = fms->edge[me_index];
    if (me.left_face == -1 || me.right_face == -1) {
      continue;
    }
    MergeFace &mf_left = fms->face[me.left_face];
    MergeFace &mf_right = fms->face[me.right_face];
    if (!dissolve_leaves_valid_bmesh(fms, me, me_index, mf_left, mf_right)) {
      continue;
    }
    if (dbg_level > 0) {
      std::cout << "Removing edge " << me_index << "\n";
    }
    splice_faces(fms, me, me_index, mf_left, mf_right);
    if (dbg_level > 1) {
      std::cout << "state after removal:\n";
      std::cout << *fms;
    }
  }
}

/* Given that tris form a triangulation of a face or part of a face that was in pm_in,
 * merge as many of the triangles together as possible, by dissolving the edges between them.
 * We can only dissolve triangulation edges that don't overlap real input edges, and we
 * can only dissolve them if doing so leaves the remaining faces able to create valid BMesh.
 * We can tell edges that don't overlap real input edges because they will have an
 * "original edge" that is different from NO_INDEX.
 */
static Vector<Facep> merge_tris_for_face(Vector<int> tris,
                                         const Mesh &tm,
                                         const Mesh &pm_in,
                                         MArena *arena)
{
  Vector<Facep> ans;
  bool done = false;
  if (tris.size() == 1) {
    ans.append(tm.face(tris[0]));
    done = true;
  }
  if (tris.size() == 2) {
    /* Is this a case where quad with one diagonal remained unchanged?
     * Worth special handling because this case will be very common.
     */
    const Face &tri1 = *tm.face(tris[0]);
    const Face &tri2 = *tm.face(tris[1]);
    Facep in_face = pm_in.face(tri1.orig);
    if (in_face->size() == 4) {
      std::pair<int, int> estarts = find_tris_common_edge(tri1, tri2);
      if (estarts.first != -1 && tri1.edge_orig[estarts.first] == NO_INDEX) {
        int i0 = estarts.first;
        int i1 = (i0 + 1) % 3;
        int i2 = (i0 + 2) % 3;
        int j2 = (estarts.second + 2) % 3;
        Face tryface({tri1[i1], tri1[i2], tri1[i0], tri2[j2]}, -1, -1, {});
        if (tryface.cyclic_equal(*in_face)) {
          ans.append(in_face);
          done = true;
        }
      }
    }
  }
  if (done) {
    return ans;
  }

  FaceMergeState fms;
  init_face_merge_state(&fms, tris, tm);
  do_dissolve(&fms);
  for (const MergeFace &mf : fms.face) {
    if (mf.merge_to == -1) {
      Array<int> e_orig(mf.edge.size());
      for (int i : mf.edge.index_range()) {
        e_orig[i] = fms.edge[mf.edge[i]].orig;
      }
      Facep facep = arena->add_face(mf.vert, mf.orig, e_orig);
      ans.append(facep);
    }
  }
  return ans;
}

/* Return an array, paralleling pm_out.vert, saying which vertices can be dissolved.
 * A vertex v can be dissolved if (a) it is not an input vertex; (b) it has valence 2;
 * and (c) if v's two neighboring vertices are u and w, then (u,v,w) forms a straight line.
 * Return the number of dissolvable vertices in r_count_dissolve.
 */
static Array<bool> find_dissolve_verts(Mesh &pm_out, int *r_count_dissolve)
{
  pm_out.populate_vert();
  /* dissolve[i] will say whether pm_out.vert(i) can be dissolved. */
  Array<bool> dissolve(pm_out.vert_size());
  for (int v_index : pm_out.vert_index_range()) {
    const Vert &vert = *pm_out.vert(v_index);
    dissolve[v_index] = (vert.orig == NO_INDEX);
  }
  /* neighbors[i] will be a pair giving the up-to-two neighboring vertices
   * of the vertex v in position i of pm_out.vert.
   * If we encounter a third, then v will not be dissolvable.
   */
  Array<std::pair<Vertp, Vertp>> neighbors(pm_out.vert_size(),
                                           std::pair<Vertp, Vertp>(nullptr, nullptr));
  for (int f : pm_out.face_index_range()) {
    const Face &face = *pm_out.face(f);
    for (int i : face.index_range()) {
      Vertp v = face[i];
      int v_index = pm_out.lookup_vert(v);
      BLI_assert(v_index != NO_INDEX);
      if (dissolve[v_index]) {
        Vertp n1 = face[face.next_pos(i)];
        Vertp n2 = face[face.prev_pos(i)];
        Vertp f_n1 = neighbors[v_index].first;
        Vertp f_n2 = neighbors[v_index].second;
        if (f_n1 != nullptr) {
          /* Already has a neighbor in another face; can't dissolve unless they are the same. */
          if (!((n1 == f_n2 && n2 == f_n1) || (n1 == f_n1 && n2 == f_n2))) {
            /* Different neighbors, so can't dissolve. */
            dissolve[v_index] = false;
          }
        }
        else {
          /* These are the first-seen neighbors. */
          neighbors[v_index] = std::pair<Vertp, Vertp>(n1, n2);
        }
      }
    }
  }
  int count = 0;
  for (int v_out : pm_out.vert_index_range()) {
    if (dissolve[v_out]) {
      dissolve[v_out] = false; /* Will set back to true if final condition is satisfied. */
      const std::pair<Vertp, Vertp> &nbrs = neighbors[v_out];
      if (nbrs.first != nullptr) {
        BLI_assert(nbrs.second != nullptr);
        const mpq3 &co1 = nbrs.first->co_exact;
        const mpq3 &co2 = nbrs.second->co_exact;
        const mpq3 &co = pm_out.vert(v_out)->co_exact;
        mpq3 dir1 = co - co1;
        mpq3 dir2 = co2 - co;
        mpq3 cross = mpq3::cross(dir1, dir2);
        if (cross[0] == 0 && cross[1] == 0 && cross[2] == 0) {
          dissolve[v_out] = true;
          ++count;
        }
      }
    }
  }
  if (r_count_dissolve != nullptr) {
    *r_count_dissolve = count;
  }
  return dissolve;
}

/* The dissolve array parallels the pm.vert array. Wherever it is true,
 * remove the corresponding vertex from the vertices in the faces of
 * pm.faces to account for the close-up of the gaps in pm.vert.
 */
static void dissolve_verts(Mesh *pm, const Array<bool> dissolve, MArena *arena)
{
  constexpr int inline_face_size = 100;
  Vector<bool, inline_face_size> face_pos_erase;
  for (int f : pm->face_index_range()) {
    const Face &face = *pm->face(f);
    face_pos_erase.clear();
    int num_erase = 0;
    for (Vertp v : face) {
      int v_index = pm->lookup_vert(v);
      BLI_assert(v_index != NO_INDEX);
      if (dissolve[v_index]) {
        face_pos_erase.append(true);
        ++num_erase;
      }
      else {
        face_pos_erase.append(false);
      }
    }
    if (num_erase > 0) {
      pm->erase_face_positions(f, face_pos_erase, arena);
    }
  }
  pm->set_dirty_verts();
}

/* The main boolean function operates on a triangle Mesh and produces a
 * Triangle Mesh as output.
 * This function converts back into a general polygonal mesh by removing
 * any possible triangulation edges (which can be identified because they
 * will have an original edge that is NO_INDEX.
 * Not all triangulation edges can be removed: if they ended up non-trivially overlapping a real
 * input edge, then we need to keep it. Also, some are necessary to make the output satisfy
 * the "valid BMesh" property: we can't produce output faces that have repeated vertices in them,
 * or have several disconnected boundaries (e.g., faces with holes).
 */
static Mesh polymesh_from_trimesh_with_dissolve(const Mesh &tm_out,
                                                const Mesh &pm_in,
                                                MArena *arena)
{
  const int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "\nPOLYMESH_FROM_TRIMESH_WITH_DISSOLVE\n";
  }
  /* Gather all output triangles that are part of each input face.
   * face_output_tris[f] will be indices of triangles in tm_out
   * that have f as their original face.
   */
  int tot_in_face = pm_in.face_size();
  Array<Vector<int>> face_output_tris(tot_in_face);
  for (int t : tm_out.face_index_range()) {
    const Face &tri = *tm_out.face(t);
    int in_face = tri.orig;
    face_output_tris[in_face].append(t);
  }
  if (dbg_level > 1) {
    std::cout << "face_output_tris:\n";
    for (int f : face_output_tris.index_range()) {
      std::cout << f << ": " << face_output_tris[f] << "\n";
    }
  }

  /* Merge triangles that we can from face_output_tri to make faces for output.
   * face_output_face[f] will be new original Facep's that
   * make up whatever part of the boolean output remains of input face f.
   */
  Array<Vector<Facep>> face_output_face(tot_in_face);
  int tot_out_face = 0;
  for (int in_f : pm_in.face_index_range()) {
    if (dbg_level > 1) {
      std::cout << "merge tris for face " << in_f << "\n";
    }
    int num_out_tris_for_face = face_output_tris.size();
    if (num_out_tris_for_face == 0) {
      continue;
    }
    face_output_face[in_f] = merge_tris_for_face(face_output_tris[in_f], tm_out, pm_in, arena);
    tot_out_face += face_output_face[in_f].size();
  }
  Array<Facep> face(tot_out_face);
  int out_f_index = 0;
  for (int in_f : pm_in.face_index_range()) {
    const Vector<Facep> &f_faces = face_output_face[in_f];
    if (f_faces.size() > 0) {
      std::copy(f_faces.begin(), f_faces.end(), &face[out_f_index]);
      out_f_index += f_faces.size();
    }
  }
  Mesh pm_out(face);

  /* Dissolve vertices that were (a) not original; and (b) now have valence 2 and
   * are between two other vertices that are exactly in line with them.
   * These were created because of triangulation edges that have been dissolved.
   */
  int count_dissolve;
  Array<bool> v_dissolve = find_dissolve_verts(pm_out, &count_dissolve);
  if (count_dissolve > 0) {
    dissolve_verts(&pm_out, v_dissolve, arena);
  }
  if (dbg_level > 1) {
    write_obj_mesh(pm_out, "boolean_post_dissolve");
  }

  return pm_out;
}

/*
 * This function does a boolean operation on a TriMesh with nshapes inputs.
 * All the shapes are combined in tm_in.
 * The shape_fn function should take a triangle index in tm_in and return
 * a number in the range 0 to nshapes-1, to say which shape that triangle is in.
 */
Mesh boolean_trimesh(Mesh &tm_in,
                     bool_optype op,
                     int nshapes,
                     std::function<int(int)> shape_fn,
                     bool use_self,
                     MArena *arena)
{
  constexpr int dbg_level = 0;
  if (dbg_level > 0) {
    std::cout << "BOOLEAN of " << nshapes << " operand" << (nshapes == 1 ? "" : "s")
              << " op=" << bool_optype_name(op) << "\n";
    if (dbg_level > 1) {
      std::cout << "boolean_trimesh input:\n" << tm_in;
      write_obj_mesh(tm_in, "boolean_in");
    }
  }
  if (tm_in.face_size() == 0) {
    return Mesh(tm_in);
  }
  Mesh tm_si;
  if (use_self) {
    tm_si = trimesh_self_intersect(tm_in, arena);
  }
  else {
    tm_si = trimesh_nary_intersect(tm_in, nshapes, shape_fn, use_self, arena);
  }
  if (dbg_level > 1) {
    write_obj_mesh(tm_si, "boolean_tm_si");
    std::cout << "\nboolean_tm_input after intersection:\n" << tm_si;
  }
  /* It is possible for tm_si to be empty if all the input triangles are bogus/degenerate. */
  if (tm_si.face_size() == 0 || op == BOOLEAN_NONE) {
    return tm_si;
  }
  auto si_shape_fn = [shape_fn, tm_si](int t) { return shape_fn(tm_si.face(t)->orig); };
  TriMeshTopology tm_si_topo(tm_si);
  PatchesInfo pinfo = find_patches(tm_si, tm_si_topo);
  CellsInfo cinfo = find_cells(tm_si, tm_si_topo, pinfo);
  if (!patch_cell_graph_connected(cinfo, pinfo)) {
    std::cout << "Implement me! disconnected patch/cell graph\n";
    return Mesh(tm_in);
  }
  bool pc_ok = patch_cell_graph_ok(cinfo, pinfo);
  if (!pc_ok) {
    /* TODO: if bad input can lead to this, diagnose the problem. */
    std::cout << "Something funny about input or a bug in boolean\n";
    return Mesh(tm_in);
  }
  cinfo.init_windings(nshapes);
  int c_ambient = find_ambient_cell(tm_si, tm_si_topo, pinfo, arena);
  if (c_ambient == NO_INDEX) {
    /* TODO: find a way to propagate this error to user properly. */
    std::cout << "Could not find an ambient cell; input not valid?\n";
    return Mesh(tm_si);
  }
  propagate_windings_and_flag(pinfo, cinfo, c_ambient, op, nshapes, si_shape_fn);
  Mesh tm_out = extract_from_flag_diffs(tm_si, pinfo, cinfo, arena);
  if (dbg_level > 1) {
    write_obj_mesh(tm_out, "boolean_tm_output");
    std::cout << "boolean tm output:\n" << tm_out;
  }
  return tm_out;
}

/* Do the boolean operation op on the polygon mesh pm_in.
 * See the header file for a complete description.
 */
Mesh boolean_mesh(Mesh &pm,
                  bool_optype op,
                  int nshapes,
                  std::function<int(int)> shape_fn,
                  bool use_self,
                  Mesh *pm_triangulated,
                  MArena *arena)
{
  Mesh *tm_in = pm_triangulated;
  Mesh our_triangulation;
  if (tm_in == nullptr) {
    our_triangulation = triangulate_polymesh(pm, arena);
    tm_in = &our_triangulation;
  }
  Mesh tm_out = boolean_trimesh(*tm_in, op, nshapes, shape_fn, use_self, arena);
  Mesh ans = polymesh_from_trimesh_with_dissolve(tm_out, pm, arena);
  return ans;
}

}  // namespace blender::meshintersect