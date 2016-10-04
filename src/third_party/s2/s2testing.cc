// Copyright 2005 Google Inc. All Rights Reserved.

#include <stdlib.h>
#include <sys/resource.h>   // for rusage, RUSAGE_SELF
#include <limits.h>

#include <vector>
using std::vector;


#include "base/integral_types.h"
#include "base/logging.h"
#include "base/stringprintf.h"
#include "util/math/matrix3x3-inl.h"
#include "s2testing.h"
#include "s2loop.h"
#include "s2latlng.h"
#include "s2latlngrect.h"
#include "s2polygon.h"
#include "s2polyline.h"
#include "s2cellunion.h"
#include "s2cell.h"
#include "s2cap.h"
#include "strings/split.h"
#include "strings/strutil.h"

S2Testing::Random::Random() {
  srandom(4);
}

uint64 S2Testing::Random::Rand64() {
  int bits_of_rand = log2(1ULL + RAND_MAX);
  uint64 result = 0;
  for (size_t num_bits = 0; num_bits < 8 * sizeof(uint64);
       num_bits += bits_of_rand) {
    result <<= bits_of_rand;
    result += random();
  }
  return result;
}

uint32 S2Testing::Random::Rand32() {
  return Rand64() & ((1ULL << 32) - 1);
}

double S2Testing::Random::RandDouble() {
  const int NUMBITS = 53;
  return ldexp(Rand64()  & ((1ULL << NUMBITS) - 1ULL), -NUMBITS);
}

int S2Testing::Random::Uniform(int upper_bound) {
  return static_cast<uint32>(RandDouble() * upper_bound);
}

bool S2Testing::Random::OneIn(int x) {
  return Uniform(x) == 0;
}

int32 S2Testing::Random::Skewed(int max_log) {
  const int32 base = Rand32() % (max_log + 1);
  return Rand32() & ((1u << base) - 1);
}

int32 S2Testing::Random::PlusOrMinus(int32 value, float multiplier) {
  const int32 range = static_cast<int32>(value * multiplier);
  const int32 rand_val = Uniform(range * 2);
  return value - range + rand_val;
}

S2Testing::Random S2Testing::rnd;

static double ParseDouble(const string& str) {
  char* end_ptr = NULL;
  double value = strtod(str.c_str(), &end_ptr);
  CHECK(end_ptr && *end_ptr == 0) << ": str == \"" << str << "\"";
  return value;
}

void S2Testing::ParseLatLngs(string const& str, vector<S2LatLng>* latlngs) {
  vector<pair<string, string> > p;
  CHECK(DictionaryParse(str, &p)) << ": str == \"" << str << "\"";
  latlngs->clear();
  for (size_t i = 0; i < p.size(); ++i) {
    latlngs->push_back(S2LatLng::FromDegrees(ParseDouble(p[i].first),
                                             ParseDouble(p[i].second)));
  }
}

void S2Testing::ParsePoints(string const& str, vector<S2Point>* vertices) {
  vector<S2LatLng> latlngs;
  S2Testing::ParseLatLngs(str, &latlngs);
  vertices->clear();
  for (size_t i = 0; i < latlngs.size(); ++i) {
    vertices->push_back(latlngs[i].ToPoint());
  }
}

S2Point S2Testing::MakePoint(string const& str) {
  vector<S2Point> vertices;
  ParsePoints(str, &vertices);
  CHECK_EQ(vertices.size(), 1);
  return vertices[0];
}

S2LatLngRect S2Testing::MakeLatLngRect(string const& str) {
  vector<S2LatLng> latlngs;
  ParseLatLngs(str, &latlngs);
  CHECK_GT(latlngs.size(), 0);
  S2LatLngRect rect = S2LatLngRect::FromPoint(latlngs[0]);
  for (size_t i = 1; i < latlngs.size(); ++i) {
    rect.AddPoint(latlngs[i]);
  }
  return rect;
}

S2Loop* S2Testing::MakeLoop(string const& str) {
  vector<S2Point> vertices;
  ParsePoints(str, &vertices);
  return new S2Loop(vertices);
}

S2Polyline* S2Testing::MakePolyline(string const& str) {
  vector<S2Point> vertices;
  ParsePoints(str, &vertices);
  return new S2Polyline(vertices);
}

S2Polygon* S2Testing::MakePolygon(string const& str) {
  vector<string> loop_strs;
  SplitStringUsing(str, ";", &loop_strs);
  vector<S2Loop*> loops;
  for (size_t i = 0; i < loop_strs.size(); ++i) {
    S2Loop* loop = MakeLoop(loop_strs[i]);
    loop->Normalize();
    loops.push_back(loop);
  }
  return new S2Polygon(&loops);  // Takes ownership.
}


S2Loop* S2Testing::MakeRegularLoop(S2Point const& center,
                                          int num_vertices,
                                          double angle_radius) {
  Matrix3x3_d m;
  S2::GetFrame(center, &m);
  vector<S2Point> vertices;
  double radian_step = 2 * M_PI / num_vertices;
  // We create the vertices on the plane tangent to center, so the
  // radius on that plane is larger.
  double planar_radius = tan(angle_radius);
  for (int vi = 0; vi < num_vertices; ++vi) {
    double angle = vi * radian_step;
    S2Point p(planar_radius * cos(angle), planar_radius * sin(angle), 1);
    vertices.push_back(S2::FromFrame(m, p.Normalize()));
  }
  return new S2Loop(vertices);
}

static void AppendVertex(S2Point const& p, string* out) {
  S2LatLng ll(p);
  StringAppendF(out, "%.17g:%.17g", ll.lat().degrees(), ll.lng().degrees());
}

static void AppendVertices(S2Point const* v, int n, string* out) {
  for (int i = 0; i < n; ++i) {
    if (i > 0) *out += ", ";
    AppendVertex(v[i], out);
  }
}

string S2Testing::ToString(S2Point const& point) {
  string out;
  AppendVertex(point, &out);
  return out;
}

string S2Testing::ToString(S2LatLngRect const& rect) {
  string out;
  AppendVertex(rect.lo().ToPoint(), &out);
  out += ", ";
  AppendVertex(rect.hi().ToPoint(), &out);
  return out;
}

string S2Testing::ToString(S2Loop const* loop) {
  string out;
  AppendVertices(&loop->vertex(0), loop->num_vertices(), &out);
  return out;
}

string S2Testing::ToString(S2Polyline const* polyline) {
  string out;
  AppendVertices(&polyline->vertex(0), polyline->num_vertices(), &out);
  return out;
}

string S2Testing::ToString(S2Polygon const* polygon) {
  string out;
  for (int i = 0; i < polygon->num_loops(); ++i) {
    if (i > 0) out += ";\n";
    S2Loop* loop = polygon->loop(i);
    AppendVertices(&loop->vertex(0), loop->num_vertices(), &out);
  }
  return out;
}

void DumpLoop(S2Loop const* loop) {
  // Only for calling from a debugger.
  cout << S2Testing::ToString(loop) << "\n";
}

void DumpPolyline(S2Polyline const* polyline) {
  // Only for calling from a debugger.
  cout << S2Testing::ToString(polyline) << "\n";
}

void DumpPolygon(S2Polygon const* polygon) {
  // Only for calling from a debugger.
  cout << S2Testing::ToString(polygon) << "\n";
}

S2Point S2Testing::RandomPoint() {
  // The order of evaluation of function arguments is unspecified,
  // so we may not just call S2Point with three RandDouble-based args.
  // Use temporaries to induce sequence points between calls.
  double x = 2 * rnd.RandDouble() - 1;
  double y = 2 * rnd.RandDouble() - 1;
  double z = 2 * rnd.RandDouble() - 1;
  return S2Point(x, y, z).Normalize();
}

void S2Testing::GetRandomFrame(S2Point* x, S2Point* y, S2Point* z) {
  *x = RandomPoint();
  *y = x->CrossProd(RandomPoint()).Normalize();
  *z = x->CrossProd(*y).Normalize();
}

S2CellId S2Testing::GetRandomCellId(int level) {
  int face = rnd.Uniform(S2CellId::kNumFaces);
  uint64 pos = rnd.Rand64() & ((1ULL << (2 * S2CellId::kMaxLevel)) - 1);
  return S2CellId::FromFacePosLevel(face, pos, level);
}

S2CellId S2Testing::GetRandomCellId() {
  return GetRandomCellId(rnd.Uniform(S2CellId::kMaxLevel + 1));
}

S2Cap S2Testing::GetRandomCap(double min_area, double max_area) {
  double cap_area = max_area * pow(min_area / max_area, rnd.RandDouble());
  DCHECK_GE(cap_area, min_area);
  DCHECK_LE(cap_area, max_area);

  // The surface area of a cap is 2*Pi times its height.
  return S2Cap::FromAxisArea(RandomPoint(), cap_area);
}

S2Point S2Testing::SamplePoint(S2Cap const& cap) {
  // We consider the cap axis to be the "z" axis.  We choose two other axes to
  // complete the coordinate frame.

  Matrix3x3_d m;
  S2::GetFrame(cap.axis(), &m);

  // The surface area of a spherical cap is directly proportional to its
  // height.  First we choose a random height, and then we choose a random
  // point along the circle at that height.

  double h = rnd.RandDouble() * cap.height();
  double theta = 2 * M_PI * rnd.RandDouble();
  double r = sqrt(h * (2 - h));  // Radius of circle.

  // The result should already be very close to unit-length, but we might as
  // well make it accurate as possible.
  return S2::FromFrame(m, S2Point(cos(theta) * r, sin(theta) * r, 1 - h))
         .Normalize();
}

void S2Testing::CheckCovering(S2Region const& region,
                              S2CellUnion const& covering,
                              bool check_tight,
                              S2CellId const& id) {
  if (!id.is_valid()) {
    for (int face = 0; face < 6; ++face) {
      CheckCovering(region, covering, check_tight,
                    S2CellId::FromFacePosLevel(face, 0, 0));
    }
    return;
  }

  if (!region.MayIntersect(S2Cell(id))) {
    // If region does not intersect id, then neither should the covering.
    if (check_tight) {
        CHECK(!covering.Intersects(id));
    }
  } else if (!covering.Contains(id)) {
    // The region may intersect id, but we can't assert that the covering
    // intersects id because we may discover that the region does not actually
    // intersect upon further subdivision.  (MayIntersect is not exact.)
    CHECK(!region.Contains(S2Cell(id)));
    CHECK(!id.is_leaf());
    S2CellId end = id.child_end();
    S2CellId child;
    for (child = id.child_begin(); child != end; child = child.next()) {
      CheckCovering(region, covering, check_tight, child);
    }
  }
}

double S2Testing::GetCpuTime() {
  struct rusage ru;
  CHECK_EQ(getrusage(RUSAGE_SELF, &ru), 0);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6;
}
