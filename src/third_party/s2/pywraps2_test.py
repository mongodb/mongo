#!/usr/bin/python2.4
# Copyright 2006 Google Inc. All Rights Reserved.



from google3.pyglib import app
from google3.testing.pybase import googletest
from google3.util.geometry.pywraps2 import *

class PyWrapS2TestCase(googletest.TestCase):

  def testContainsIsWrappedCorrectly(self):
    london = S2LatLngRect(S2LatLng.FromDegrees(51.3368602, 0.4931979),
                          S2LatLng.FromDegrees(51.7323965, 0.1495211))
    e14lj = S2LatLngRect(S2LatLng.FromDegrees(51.5213527, -0.0476026),
                         S2LatLng.FromDegrees(51.5213527, -0.0476026))
    self.failUnless(london.Contains(e14lj))

  def testS2CellIdEqualsIsWrappedCorrectly(self):
    london = S2LatLng.FromDegrees(51.5001525, -0.1262355)
    cell = S2CellId.FromLatLng(london)
    same_cell = S2CellId.FromLatLng(london)
    self.assertEquals(cell, same_cell)

  def testS2CellIdComparsionIsWrappedCorrectly(self):
    london = S2LatLng.FromDegrees(51.5001525, -0.1262355)
    cell = S2CellId.FromLatLng(london)
    self.failUnless(cell < cell.next())
    self.failUnless(cell.next() > cell)

  def testS2HashingIsWrappedCorrectly(self):
    london = S2LatLng.FromDegrees(51.5001525, -0.1262355)
    cell = S2CellId.FromLatLng(london)
    same_cell = S2CellId.FromLatLng(london)
    self.assertEquals(hash(cell), hash(same_cell))

  def testCovererIsWrapperCorrectly(self):
    london = S2LatLngRect(S2LatLng.FromDegrees(51.3368602, 0.4931979),
                          S2LatLng.FromDegrees(51.7323965, 0.1495211))
    e14lj = S2LatLngRect(S2LatLng.FromDegrees(51.5213527, -0.0476026),
                         S2LatLng.FromDegrees(51.5213527, -0.0476026))
    coverer = S2RegionCoverer()
    covering = coverer.GetCovering(e14lj)
    for cellid in covering:
      self.failUnless(london.Contains(S2Cell(cellid)))
    
def main(argv):
  googletest.main()

if __name__ == "__main__":
  app.run()
