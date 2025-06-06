/******************************************************************************
 *
 * Project:  KML Driver
 * Purpose:  KML driver utilities
 * Author:   Jens Oberender, j.obi@troja.net
 *
 ******************************************************************************
 * Copyright (c) 2007, Jens Oberender
 * Copyright (c) 2009, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/
#ifndef OGR_KMLUTILITY_H_INCLUDED
#define OGR_KMLUTILITY_H_INCLUDED

#include <string>
#include <vector>
#include "ogr_geometry.h"

namespace OGRKML
{

enum Nodetype
{
    Unknown,
    Empty,
    Mixed,
    Point,
    LineString,
    Polygon,
    Rest,
    MultiGeometry,
    MultiPoint,
    MultiLineString,
    MultiPolygon
};

struct Attribute
{
    std::string sName{};
    std::string sValue{};
};

struct Coordinate
{
    double dfLongitude = 0;
    double dfLatitude = 0;
    double dfAltitude = 0;
    bool bHasZ = false;
};

struct Feature
{
    Nodetype eType = Unknown;
    std::string sName{};
    std::string sDescription{};
    std::unique_ptr<OGRGeometry> poGeom{};
};

}  // namespace OGRKML

using namespace OGRKML;

#endif /* OGR_KMLUTILITY_H_INCLUDED */
