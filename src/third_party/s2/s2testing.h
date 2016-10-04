// Copyright 2005 Google Inc. All Rights Reserved.

#ifndef UTIL_GEOMETRY_S2TESTING_H__
#define UTIL_GEOMETRY_S2TESTING_H__

#include <string>
using std::string;

#include <vector>
using std::vector;

#include "s2.h"
#include "s2cellid.h"

class S2LatLngRect;
class S2Loop;
class S2Polygon;
class S2Polyline;
class S2Region;
class S2CellUnion;
class S2Cap;

// This class defines various static functions that are useful for writing
// unit tests.
class S2Testing {
 public:
  class Random;

  // Given a latitude-longitude coordinate in degrees,
  // return a newly allocated point.  Example of the input format:
  //     "-20:150"
  static S2Point MakePoint(string const& str);

  // Given a string of one or more latitude-longitude coordinates in degrees,
  // return the minimal bounding S2LatLngRect that contains the coordinates.
  // Example of the input format:
  //     "-20:150"                     // one point
  //     "-20:150, -20:151, -19:150"   // three points
  static S2LatLngRect MakeLatLngRect(string const& str);

  // Given a string of latitude-longitude coordinates in degrees,
  // return a newly allocated loop.  Example of the input format:
  //     "-20:150, 10:-120, 0.123:-170.652"
  static S2Loop* MakeLoop(string const& str);

  // Similar to MakeLoop(), but returns an S2Polyline rather than an S2Loop.
  static S2Polyline* MakePolyline(string const& str);

  // Given a sequence of loops separated by semicolons, return a newly
  // allocated polygon.  Loops are automatically inverted if necessary so
  // that they enclose at most half of the unit sphere.
  static S2Polygon* MakePolygon(string const& str);


  // Returns a newly allocated loop (owned by caller) shaped as a
  // regular polygon with num_vertices vertices, all on a circle of
  // radius radius_angle around the center.  The radius is the actual
  // distance from the center to the circle along the sphere.
  static S2Loop* MakeRegularLoop(S2Point const& center,
                                 int num_vertices,
                                 double radius_angle);

  // Examples of the input format:
  //     "10:20, 90:0, 20:30"                                  // one loop
  //     "10:20, 90:0, 20:30; 5.5:6.5, -90:-180, -15.2:20.3"   // two loops

  // Parse a string in the same format as MakeLatLngRect, and return the
  // corresponding vector of S2LatLng points.
  static void ParseLatLngs(string const& str, vector<S2LatLng>* latlngs);

  // Parse a string in the same format as MakeLatLngRect, and return the
  // corresponding vector of S2Point values.
  static void ParsePoints(string const& str, vector<S2Point>* vertices);

  // Convert a point, lat-lng rect, loop, polyline, or polygon to the string
  // format above.
  static string ToString(S2Point const& point);
  static string ToString(S2LatLngRect const& rect);
  static string ToString(S2Loop const* loop);
  static string ToString(S2Polyline const* polyline);
  static string ToString(S2Polygon const* polygon);

  // A deterministically-seeded random number generator.
  static Random rnd;

  // Return a random unit-length vector.
  static S2Point RandomPoint();

  // Return a right-handed coordinate frame (three orthonormal vectors).
  static void GetRandomFrame(S2Point* x, S2Point* y, S2Point* z);

  // Return a cap with a random axis such that the log of its area is
  // uniformly distributed between the logs of the two given values.
  // (The log of the cap angle is also approximately uniformly distributed.)
  static S2Cap GetRandomCap(double min_area, double max_area);

  // Return a point chosen uniformly at random (with respect to area)
  // from the given cap.
  static S2Point SamplePoint(S2Cap const& cap);

  // Return a random cell id at the given level or at a randomly chosen
  // level.  The distribution is uniform over the space of cell ids,
  // but only approximately uniform over the surface of the sphere.
  static S2CellId GetRandomCellId(int level);
  static S2CellId GetRandomCellId();

  // Checks that "covering" completely covers the given region.  If
  // "check_tight" is true, also checks that it does not contain any cells
  // that do not intersect the given region.  ("id" is only used internally.)
  static void CheckCovering(S2Region const& region,
                            S2CellUnion const& covering,
                            bool check_tight,
                            S2CellId const& id = S2CellId());

  // Returns the user time consumed by this process, in seconds.
  static double GetCpuTime();
};

// Functions in this class return random numbers that are as good as
// rand() is.  The results will be reproducible as the seed is
// deterministic.
class S2Testing::Random {
 public:
  Random();
  uint64 Rand64();
  uint32 Rand32();
  double RandDouble();
  int32 Uniform(int32 upper_bound);
  int32 operator() (int32 n) {
    return Uniform(n);
  }
  bool OneIn(int x);

  // Skewed: pick "base" uniformly from range [0,max_log] and then
  // return "base" random bits.  The effect is to pick a number in the
  // range [0,2^max_log-1] with bias towards smaller numbers.
  int32 Skewed(int max_log);

  // PlusOrMinus: return a uniformly distributed value in the range
  // [value - (value * multiplier), value + (value * multiplier) )
  // (i.e. inclusive on the lower end and exclusive on the upper end).
  //
  // Be careful of floating point rounding, e.g., 1.0/29 is inexactly
  // represented, and so we get:
  //
  // PlusOrMinus(2 * 29, 1.0/29) is
  // PlusOrMinus(58, 0.0344827849223) which gives
  // range = static_cast<int32>(1.999999992549) = 1, rand_val \in [0, 2)
  // and return result \in [57, 59) rather than [56, 60) as probably
  // intended.  (This holds for IEEE754 floating point.)
  int32 PlusOrMinus(int32 value, float multiplier);
};

#endif  // UTIL_GEOMETRY_S2TESTING_H__
