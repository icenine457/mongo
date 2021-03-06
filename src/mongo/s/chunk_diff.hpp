// @file chunk_diff_impl.hpp

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "chunk_diff.h"

namespace mongo {

    template < class ValType, class ShardType >
    bool ConfigDiffTracker<ValType,ShardType>::
        isOverlapping( const BSONObj& min, const BSONObj& max )
    {
        RangeOverlap overlap = overlappingRange( min, max );

        return overlap.first != overlap.second;
    }

    template < class ValType, class ShardType >
    void ConfigDiffTracker<ValType,ShardType>::
        removeOverlapping( const BSONObj& min, const BSONObj& max )
    {
        verifyAttached();

        RangeOverlap overlap = overlappingRange( min, max );

        _currMap->erase( overlap.first, overlap.second );
    }

    template < class ValType, class ShardType >
    typename ConfigDiffTracker<ValType,ShardType>::RangeOverlap ConfigDiffTracker<ValType,ShardType>::
        overlappingRange( const BSONObj& min, const BSONObj& max )
    {
        verifyAttached();

        typename RangeMap::iterator low;
        typename RangeMap::iterator high;

        if( isMinKeyIndexed() ){
            // Returns the first chunk with a min key that is >= min - implies the
            // previous chunk cannot overlap min
            low = _currMap->lower_bound( min );
            // Returns the first chunk with a min key that is >= max - implies the
            // chunk does not overlap max
            high = _currMap->lower_bound( max );
        }
        else{
            // Returns the first chunk with a max key that is > min - implies that
            // the chunk overlaps min
            low = _currMap->upper_bound( min );
            // Returns the first chunk with a max key that is > max - implies that
            // the next chunk cannot not overlap max
            high = _currMap->upper_bound( max );
        }

        return RangeOverlap( low, high );
    }

    template < class ValType, class ShardType >
    int ConfigDiffTracker<ValType,ShardType>::
        calculateConfigDiff( string config,
                             const set<ShardChunkVersion>& extraMinorVersions )
    {
        verifyAttached();

        // Get the diff query required
        Query diffQuery = configDiffQuery( extraMinorVersions );

        scoped_ptr<ScopedDbConnection> conn( ScopedDbConnection::getScopedDbConnection( config ) );

        try {

            // Open a cursor for the diff chunks
            auto_ptr<DBClientCursor> cursor = conn->get()->query(
                    ShardNS::chunk, diffQuery, 0, 0, 0, 0, ( DEBUG_BUILD ? 2 : 1000000 ) );
            verify( cursor.get() );

            int diff = calculateConfigDiff( *cursor.get() );

            conn->done();

            return diff;
        }
        catch( DBException& e ){
            // Should only happen on connection errors
            e.addContext( str::stream() << "could not calculate config difference for ns " << _ns << " on " << config );
            throw;
        }
    }

    template < class ValType, class ShardType >
    int ConfigDiffTracker<ValType,ShardType>::
        calculateConfigDiff( DBClientCursorInterface& diffCursor )
    {
        verifyAttached();

        // Apply the chunk changes to the ranges and versions

        //
        // Overall idea here is to work in two steps :
        // 1. For all the new chunks we find, increment the maximum version per-shard and
        //    per-collection, and remove any conflicting chunks from the ranges
        // 2. For all the new chunks we're interested in (all of them for mongos, just chunks on the
        //    shard for mongod) add them to the ranges
        //

        vector<BSONObj> newTracked;

        int chunksFound = 0;
        while( diffCursor.more() ){

            BSONObj diffChunkDoc = diffCursor.next();
            chunksFound++;

            if( diffChunkDoc[ "min" ].type() != Object || diffChunkDoc[ "max" ].type() != Object ||
                diffChunkDoc[ "lastmod" ].type() != Timestamp || diffChunkDoc[ "shard" ].type() != String )
            {
                warning() << "got invalid chunk document " << diffChunkDoc << " when trying to remove overlapping chunks" << endl;
                continue;
            }

            // Get max changed version and chunk version
            ShardChunkVersion chunkVersion = ShardChunkVersion::fromBSON( diffChunkDoc[ "lastmod" ] );
            if( chunkVersion > *_maxVersion ) *_maxVersion = chunkVersion;

            // Chunk version changes
            ShardType shard = shardFor( diffChunkDoc[ "shard" ].String() );
            typename map<ShardType, ShardChunkVersion>::iterator shardVersionIt = _maxShardVersions->find( shard );
            if( shardVersionIt == _maxShardVersions->end() || shardVersionIt->second < chunkVersion ){
                (*_maxShardVersions)[ shard ] = chunkVersion;
            }

            // See if we need to remove any chunks we are currently tracking b/c of this chunk's changes
            removeOverlapping( diffChunkDoc[ "min" ].Obj(), diffChunkDoc[ "max" ].Obj() );

            // Figure out which of the new chunks we need to track
            // Important - we need to actually own this doc, in case the cursor decides to getMore or unbuffer
            if( isTracked( diffChunkDoc ) ) newTracked.push_back( diffChunkDoc.getOwned() );
        }

        LOG(3) << "found " << chunksFound << " new chunks for collection " << _ns
               << " (tracking " << newTracked.size() << "), new version is " << _maxVersion << endl;

        for( vector<BSONObj>::iterator it = newTracked.begin(); it != newTracked.end(); it++ ){

            BSONObj chunkDoc = *it;

            // Important - we need to make sure we actually own the min and max here
            BSONObj min = chunkDoc[ "min" ].Obj().getOwned();
            BSONObj max = chunkDoc[ "max" ].Obj().getOwned();

            // Invariant enforced by sharding
            // It's possible to read inconsistent state b/c of getMore() and yielding, so we want
            // to detect as early as possible.
            // TODO: This checks for overlap, we also should check for holes here iff we're tracking
            // all chunks
            if( isOverlapping( min, max ) ) return -1;

            _currMap->insert( rangeFor( chunkDoc, min, max ) );
        }

        return chunksFound;
    }

    template < class ValType, class ShardType >
    Query ConfigDiffTracker<ValType,ShardType>::
        configDiffQuery( const set<ShardChunkVersion>& extraMinorVersions ) const
    {
        verifyAttached();

        //
        // Basic idea behind the query is to find all the chunks $gt the current max version, and
        // then also update chunks that we need minor versions for - i.e. max chunks on shards and
        // extra versions we pass in (for splits)
        //

        BSONObjBuilder queryB;
        {
            BSONArrayBuilder queryOrB( queryB.subarrayStart( "$or" ) );

            // Get any version changes higher than we know currently
            {
                BSONObjBuilder queryNewB( queryOrB.subobjStart() );

                queryNewB.append( "ns", _ns );
                {
                    BSONObjBuilder ts( queryNewB.subobjStart( "lastmod" ) );
                    // We should *always* pull at least a single chunk back, this lets us quickly
                    // detect if our collection was unsharded (and most of the time if it was
                    // resharded) in the meantime
                    ts.appendTimestamp( "$gte", _maxVersion->toLong() );
                    ts.done();
                }

                queryNewB.done();
            }

            // Get any shard version changes higher than we know currently
            // Needed since there could have been a split of the max version chunk of any shard
            // TODO: Ideally, we shouldn't care about these
            for( typename map<ShardType, ShardChunkVersion>::const_iterator it = _maxShardVersions->begin(); it != _maxShardVersions->end(); it++ ){
                BSONObjBuilder queryShardB( queryOrB.subobjStart() );

                queryShardB.append( "ns", _ns );
                queryShardB.append( "shard", nameFrom( it->first ) );
                {
                    BSONObjBuilder ts( queryShardB.subobjStart( "lastmod" ) );
                    ts.appendTimestamp( "$gt", it->second.toLong() );
                    ts.done();
                }
                queryShardB.done();
            }

            // Get any minor version changes we've marked as interesting
            // TODO: Ideally, we shouldn't care about these
            for( set<ShardChunkVersion>::const_iterator it = extraMinorVersions.begin(); it != extraMinorVersions.end(); it++ ){
                BSONObjBuilder queryShardB( queryOrB.subobjStart() );

                queryShardB.append( "ns", _ns );
                {
                    BSONObjBuilder ts( queryShardB.subobjStart( "lastmod" ) );
                    ts.appendTimestamp( "$gt", it->toLong() );
                    ts.appendTimestamp( "$lt",
                                        ShardChunkVersion( it->majorVersion() + 1, 0, OID() ).toLong() );
                    ts.done();
                }
                queryShardB.done();
            }

            queryOrB.done();
        }

        BSONObj query = queryB.obj();

        // log() << "major version query from " << *_maxVersion << " and over " << _maxShardVersions->size() << " shards is " << query << endl;

        return Query( query );
    }

} // namespace mongo

