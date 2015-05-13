/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "GeometryPool"
#include <osgEarth/Locators>
#include <osg/Point>
#include <cstdlib> // for getenv

using namespace osgEarth;
using namespace osgEarth::Drivers::RexTerrainEngine;

#define LC "[GeometryPool] "

// TODO: experiment with sharing a single texture coordinate array 
//// across all shared geometries.
#define SHARE_TEX_COORDS 1


GeometryPool::GeometryPool(unsigned                       tileSize,
                           const RexTerrainEngineOptions& options) :
_tileSize( tileSize ),
_options ( options ),
_debug   ( false )
{
    if ( getenv("OSGEARTH_DEBUG_REX_GEOMETRY_POOL") != 0L )
    {
        _debug = true;
    }
}

void
GeometryPool::getPooledGeometry(const TileKey&               tileKey,
                                const MapInfo&               mapInfo,
                                osg::ref_ptr<osg::Geometry>& out)
{
    // convert to a unique-geometry key:
    GeometryKey geomKey;
    createKeyForTileKey( tileKey, mapInfo, geomKey );

    // Look it up in the pool:
    Threading::ScopedMutexLock exclusive( _geometryMapMutex );

    GeometryMap::iterator i = _geometryMap.find( geomKey );
    if ( i != _geometryMap.end() )
    {
        // Found. return it.
        out = i->second.get();
    }
    else
    {
        // Not found. Create it.
        out = createGeometry( tileKey, mapInfo, 0L );
        _geometryMap[ geomKey ] = out.get();

        if ( _debug )
        {
            OE_NOTICE << LC << "Geometry pool size = " << _geometryMap.size() << "\n";
        }
    }
}

void
GeometryPool::createKeyForTileKey(const TileKey&             tileKey,
                                  const MapInfo&             mapInfo,
                                  GeometryPool::GeometryKey& out) const
{
    out.lod  = tileKey.getLOD();
    out.yMin = mapInfo.isGeocentric()? tileKey.getExtent().yMin() : 0.0;
}

osg::Geometry*
GeometryPool::createGeometry(const TileKey& tileKey,
                             const MapInfo& mapInfo,
                             MaskGenerator* maskSet) const
{
    if ( _options.gpuTessellation() == true )
    {
        return createPatchGeometry(tileKey, mapInfo, maskSet);
    }
    else
    {
        return createTriangleGeometry(tileKey, mapInfo, maskSet); 
    }
}

#define addSkirtDataForIndex(INDEX, HEIGHT) \
{ \
    verts->push_back( (*verts)[INDEX] ); \
    normals->push_back( (*normals)[INDEX] ); \
    texCoords->push_back( (*texCoords)[INDEX] ); \
    verts->push_back( (*verts)[INDEX] - ((*normals)[INDEX])*(HEIGHT) ); \
    normals->push_back( (*normals)[INDEX] ); \
    texCoords->push_back( (*texCoords)[INDEX] ); \
}

#define addSkirtTriangles(INDEX0, INDEX1) \
{ \
    if ( maskSet == 0L || (!maskSet->isMasked((*texCoords)[INDEX0]) && !maskSet->isMasked((*texCoords)[INDEX1])) ) \
    { \
        primSet->addElement((INDEX0));   \
        primSet->addElement((INDEX0)+1); \
        primSet->addElement((INDEX1));   \
        primSet->addElement((INDEX1));   \
        primSet->addElement((INDEX0)+1); \
        primSet->addElement((INDEX1)+1); \
    } \
}

osg::Geometry*
GeometryPool::createPatchGeometry(const TileKey& tileKey,
                                  const MapInfo& mapInfo,
                                  MaskGenerator* maskSet) const
{
    osg::ref_ptr<GeoLocator> locator = GeoLocator::createForKey( tileKey, mapInfo );

    osg::BoundingSphere tileBound;
    
    // Establish a local reference frame for the tile:
    osg::Vec3d centerWorld;
    GeoPoint centroid;
    tileKey.getExtent().getCentroid( centroid );
    centroid.toWorld( centerWorld );

    osg::Matrix world2local, local2world;
    centroid.createWorldToLocal( world2local );
    local2world.invert( world2local );

    const unsigned numRows = 4;
    const unsigned numCols = 4;
    unsigned numVerts = numRows * numCols;
    
    // the geometry:
    osg::Geometry* geom = new osg::Geometry();
    geom->setUseVertexBufferObjects(true);
    geom->setUseDisplayList(false);

    // the vertex locations:
    osg::Vec3Array* verts = new osg::Vec3Array();
    verts->reserve( numVerts );
    geom->setVertexArray( verts );

    // the surface normals (i.e. extrusion vectors)
    osg::Vec3Array* normals = new osg::Vec3Array();
    normals->reserve( numVerts );
    geom->setNormalArray( normals );
    geom->setNormalBinding( geom->BIND_PER_VERTEX );

#ifdef SHARE_TEX_COORDS
    bool populateTexCoords = false;
    if ( !_sharedTexCoords.valid() )
    {
        _sharedTexCoords = new osg::Vec3Array();
        _sharedTexCoords->reserve( numVerts );
        populateTexCoords = true;
    }    
    osg::Vec3Array* texCoords = _sharedTexCoords.get();
#else
    bool populateTexCoords = true;
    osg::Vec3Array* texCoords = new osg::Vec3Array();
    texCoords->reserve( numVerts );
#endif

    geom->setTexCoordArray( 0, texCoords );

    for(unsigned row=0; row<numRows; ++row)
    {
        float ny = (float)row/(float)(numRows-1);

        for(unsigned col=0; col<numCols; ++col)
        {
            float nx = (float)col/(float)(numCols-1);

            osg::Vec3d model;
            locator->unitToModel(osg::Vec3d(nx, ny, 0.0), model);
            osg::Vec3d modelLTP = model*world2local;
            verts->push_back( modelLTP );
            tileBound.expandBy( verts->back() );

            // no masking in the geometry pool, so always write a z=1.0 -gw
            //bool masked = _maskSet.contains(nx, ny);     
            if ( populateTexCoords )
            {
                texCoords->push_back( osg::Vec3f(nx, ny, 1.0f) );
            }

            osg::Vec3d modelPlusOne;
            locator->unitToModel(osg::Vec3d(nx, ny, 1.0f), modelPlusOne);
            osg::Vec3f normal = (modelPlusOne*world2local)-modelLTP;
            normal.normalize();
            normals->push_back( normal );
        }
    }
    
    osg::DrawArrays* patch = new osg::DrawArrays(GL_PATCHES, 0, numVerts);
    geom->addPrimitiveSet( patch );

    return geom;
}

osg::Geometry*
GeometryPool::createTriangleGeometry(const TileKey& tileKey,
                                     const MapInfo& mapInfo,
                                     MaskGenerator* maskSet) const
{
    osg::ref_ptr<GeoLocator> locator = GeoLocator::createForKey( tileKey, mapInfo );
    
    // Establish a local reference frame for the tile:
    osg::Vec3d centerWorld;
    GeoPoint centroid;
    tileKey.getExtent().getCentroid( centroid );
    centroid.toWorld( centerWorld );

    osg::Matrix world2local, local2world;
    centroid.createWorldToLocal( world2local );
    local2world.invert( world2local );

    // Attempt to calculate the number of verts in the surface geometry.
    bool createSkirt = _options.heightFieldSkirtRatio() > 0.0f;
    unsigned numVertsInSurface = (_tileSize*_tileSize);
    unsigned numVertsInSkirt = createSkirt ? _tileSize*4u - 4u : 0;
    unsigned numVerts = numVertsInSurface + numVertsInSkirt;    
    unsigned numIndiciesInSurface = (_tileSize-1) * (_tileSize-1) * 6;
    unsigned numIncidesInSkirt    = createSkirt ? (_tileSize-1) * 4 * 6 : 0;
    
    // Pre-allocate enought space for all triangles.
    osg::DrawElements* primSet = numVerts > 0xFFFF ? 
        (osg::DrawElements*)(new osg::DrawElementsUInt(GL_TRIANGLES)) :
        (osg::DrawElements*)(new osg::DrawElementsUShort(GL_TRIANGLES));

    primSet->reserveElements(numIndiciesInSurface + numIncidesInSkirt);

    osg::BoundingSphere tileBound;

    // the geometry:
    osg::Geometry* geom = new osg::Geometry();
    geom->setUseVertexBufferObjects(true);
    geom->setUseDisplayList(false);

    geom->addPrimitiveSet( primSet );

    // the vertex locations:
    osg::Vec3Array* verts = new osg::Vec3Array();
    verts->reserve( numVerts );
    geom->setVertexArray( verts );

    // the surface normals (i.e. extrusion vectors)
    osg::Vec3Array* normals = new osg::Vec3Array();
    normals->reserve( numVerts );
    geom->setNormalArray( normals );
    geom->setNormalBinding( geom->BIND_PER_VERTEX );

#if 0
    // colors
    osg::Vec4Array* colors = new osg::Vec4Array();
    colors->push_back( osg::Vec4(1,1,1,1) );
    geom->setColorArray( colors );
    geom->setColorBinding( geom->BIND_OVERALL );
#endif

    // tex coord is [0..1] across the tile. The 3rd dimension tracks whether the
    // vert is masked: 0=yes, 1=no
#ifdef SHARE_TEX_COORDS
    bool populateTexCoords = false;
    if ( !_sharedTexCoords.valid() )
    {
        _sharedTexCoords = new osg::Vec3Array();
        _sharedTexCoords->reserve( numVerts );
        populateTexCoords = true;
    }    
    osg::Vec3Array* texCoords = _sharedTexCoords.get();
#else
    bool populateTexCoords = true;
    osg::Vec3Array* texCoords = new osg::Vec3Array();
    texCoords->reserve( numVerts );
#endif

    geom->setTexCoordArray( 0, texCoords );
    
    for(unsigned row=0; row<_tileSize; ++row)
    {
        float ny = (float)row/(float)(_tileSize-1);
        for(unsigned col=0; col<_tileSize; ++col)
        {
            float nx = (float)col/(float)(_tileSize-1);

            osg::Vec3d model;
            locator->unitToModel(osg::Vec3d(nx, ny, 0.0f), model);
            osg::Vec3d modelLTP = model*world2local;
            verts->push_back( modelLTP );
            tileBound.expandBy( verts->back() );

            // no masking in the geometry pool, so always write a z=1.0 -gw
            //bool masked = _maskSet.contains(nx, ny);     
            if ( populateTexCoords )
            {
                texCoords->push_back( osg::Vec3f(nx, ny, 1.0f) );
            }

            osg::Vec3d modelPlusOne;
            locator->unitToModel(osg::Vec3d(nx, ny, 1.0f), modelPlusOne);
            osg::Vec3f normal = (modelPlusOne*world2local)-modelLTP;
            normal.normalize();
            normals->push_back( normal );
        }
    }

    // Now tessellate the surface.
    
    // TODO: do we really need this??
    bool swapOrientation = !locator->orientationOpenGL();

    for(unsigned j=0; j<_tileSize-1; ++j)
    {
        for(unsigned i=0; i<_tileSize-1; ++i)
        {
            int i00;
            int i01;
            if (swapOrientation)
            {
                i01 = j*_tileSize + i;
                i00 = i01+_tileSize;
            }
            else
            {
                i00 = j*_tileSize + i;
                i01 = i00+_tileSize;
            }

            int i10 = i00+1;
            int i11 = i01+1;

            // If the quad does not intersect a mask, tessellate it; otherwise skip it
            // since the mask generator will take care of it.
            bool addTris = true;
            if ( maskSet )
            {
                addTris =
                    !maskSet->isMasked( (*texCoords)[i00] ) &&
                    !maskSet->isMasked( (*texCoords)[i01] ) &&
                    !maskSet->isMasked( (*texCoords)[i10] ) &&
                    !maskSet->isMasked( (*texCoords)[i11] ) &&
                    !maskSet->containedByQuadAtColRow( i, j, _tileSize );
            }

            if ( addTris )
            {
                primSet->addElement(i01);
                primSet->addElement(i00);
                primSet->addElement(i11);

                primSet->addElement(i00);
                primSet->addElement(i10);
                primSet->addElement(i11);
            }
        }
    }

    // SKIRTS:
    // calculate the skirt extrusion height
    double height = tileBound.radius() * _options.heightFieldSkirtRatio().get();
        
    unsigned skirtIndex = verts->size();

    // first, create all the skirt verts, normals, and texcoords.
    for(int c=0; c<(int)_tileSize-1; ++c)
        addSkirtDataForIndex( c, height ); //top

    for(int r=0; r<(int)_tileSize-1; ++r)
        addSkirtDataForIndex( r*_tileSize+(_tileSize-1), height ); //right
    
    for(int c=_tileSize-1; c>=0; --c)
        addSkirtDataForIndex( (_tileSize-1)*_tileSize+c, height ); //bottom

    for(int r=_tileSize-1; r>=0; --r)
        addSkirtDataForIndex( r*_tileSize, height ); //left
    
    // then create the elements indices:
    int i;
    for(i=skirtIndex; i<(int)verts->size()-2; i+=2)
        addSkirtTriangles( i, i+2 );

    addSkirtTriangles( i, skirtIndex );

    return geom;
}