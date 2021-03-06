//@file update.cpp

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

#include "pch.h"

#include "mongo/db/oplog.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/queryutil.h"
#include "mongo/client/dbclientinterface.h"

#include "update.h"
#include "update_internal.h"

//#define DEBUGUPDATE(x) cout << x << endl;
#define DEBUGUPDATE(x)

namespace mongo {

    void checkNoMods( BSONObj o ) {
        BSONObjIterator i( o );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            uassert( 10154 ,  "Modifiers and non-modifiers cannot be mixed", e.fieldName()[ 0 ] != '$' );
        }
    }

    static void checkTooLarge(const BSONObj& newObj) {
        uassert( 12522 , "$ operator made object too large" , newObj.objsize() <= BSONObjMaxUserSize );
    }

    /* note: this is only (as-is) called for

             - not multi
             - not mods is indexed
             - not upsert
    */
    static UpdateResult _updateById(bool isOperatorUpdate,
                                    int idIdxNo,
                                    ModSet* mods,
                                    int profile,
                                    NamespaceDetails* d,
                                    NamespaceDetailsTransient *nsdt,
                                    bool su,
                                    const char* ns,
                                    const BSONObj& updateobj,
                                    BSONObj patternOrig,
                                    bool logop,
                                    OpDebug& debug,
                                    bool fromMigrate = false) {

        DiskLoc loc;
        {
            IndexDetails& i = d->idx(idIdxNo);
            BSONObj key = i.getKeyFromQuery( patternOrig );
            loc = i.idxInterface().findSingle(i, i.head, key);
            if( loc.isNull() ) {
                // no upsert support in _updateById yet, so we are done.
                return UpdateResult( 0 , 0 , 0 , BSONObj() );
            }
        }
        Record* r = loc.rec();

        if ( cc().allowedToThrowPageFaultException() && ! r->likelyInPhysicalMemory() ) {
            throw PageFaultException( r );
        }

        /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
           regular ones at the moment. */
        if ( isOperatorUpdate ) {
            const BSONObj& onDisk = loc.obj();
            auto_ptr<ModSetState> mss = mods->prepare( onDisk );

            if( mss->canApplyInPlace() ) {
                mss->applyModsInPlace(true);
                DEBUGUPDATE( "\t\t\t updateById doing in place update" );
            }
            else {
                BSONObj newObj = mss->createNewFromMods();
                checkTooLarge(newObj);
                verify(nsdt);
                theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , newObj.objdata(), newObj.objsize(), debug);
            }

            if ( logop ) {
                DEV verify( mods->size() );

                BSONObj pattern = patternOrig;
                if ( mss->haveArrayDepMod() ) {
                    BSONObjBuilder patternBuilder;
                    patternBuilder.appendElements( pattern );
                    mss->appendSizeSpecForArrayDepMods( patternBuilder );
                    pattern = patternBuilder.obj();
                }

                if( mss->needOpLogRewrite() ) {
                    DEBUGUPDATE( "\t rewrite update: " << mss->getOpLogRewrite() );
                    logOp("u", ns, mss->getOpLogRewrite() ,
                          &pattern, 0, fromMigrate );
                }
                else {
                    logOp("u", ns, updateobj, &pattern, 0, fromMigrate );
                }
            }
            return UpdateResult( 1 , 1 , 1 , BSONObj() );
        } // end $operator update

        // regular update
        BSONElementManipulator::lookForTimestamps( updateobj );
        checkNoMods( updateobj );
        verify(nsdt);
        theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , updateobj.objdata(), updateobj.objsize(), debug );
        if ( logop ) {
            logOp("u", ns, updateobj, &patternOrig, 0, fromMigrate );
        }
        return UpdateResult( 1 , 0 , 1 , BSONObj() );
    }

    UpdateResult _updateObjects( bool su,
                                 const char* ns,
                                 const BSONObj& updateobj,
                                 const BSONObj& patternOrig,
                                 bool upsert,
                                 bool multi,
                                 bool logop ,
                                 OpDebug& debug,
                                 RemoveSaver* rs,
                                 bool fromMigrate,
                                 const QueryPlanSelectionPolicy& planPolicy ) {

        DEBUGUPDATE( "update: " << ns
                     << " update: " << updateobj
                     << " query: " << patternOrig
                     << " upsert: " << upsert << " multi: " << multi );

        Client& client = cc();
        int profile = client.database()->profile;

        debug.updateobj = updateobj;

        // The idea with these here it to make them loop invariant for
        // multi updates, and thus be a bit faster for that case.  The
        // pointers may be left invalid on a failed or terminal yield
        // recovery.
        NamespaceDetails* d = nsdetails(ns); // can be null if an upsert...
        NamespaceDetailsTransient* nsdt = &NamespaceDetailsTransient::get(ns);

        auto_ptr<ModSet> mods;
        bool isOperatorUpdate = updateobj.firstElementFieldName()[0] == '$';
        int modsIsIndexed = false; // really the # of indexes
        if ( isOperatorUpdate ) {
            if( d && d->indexBuildInProgress ) {
                set<string> bgKeys;
                d->inProgIdx().keyPattern().getFieldNames(bgKeys);
                mods.reset( new ModSet(updateobj, nsdt->indexKeys(), &bgKeys) );
            }
            else {
                mods.reset( new ModSet(updateobj, nsdt->indexKeys()) );
            }
            modsIsIndexed = mods->isIndexed();
        }

        if( planPolicy.permitOptimalIdPlan() && !multi && isSimpleIdQuery(patternOrig) && d &&
           !modsIsIndexed ) {
            int idxNo = d->findIdIndex();
            if( idxNo >= 0 ) {
                debug.idhack = true;

                UpdateResult result = _updateById( isOperatorUpdate,
                                                   idxNo,
                                                   mods.get(),
                                                   profile,
                                                   d,
                                                   nsdt,
                                                   su,
                                                   ns,
                                                   updateobj,
                                                   patternOrig,
                                                   logop,
                                                   debug,
                                                   fromMigrate);
                if ( result.existing || ! upsert ) {
                    return result;
                }
                else if ( upsert && ! isOperatorUpdate && ! logop) {
                    // this handles repl inserts
                    checkNoMods( updateobj );
                    debug.upsert = true;
                    BSONObj no = updateobj;
                    theDataFileMgr.insertWithObjMod(ns, no, su);
                    return UpdateResult( 0 , 0 , 1 , no );
                }
            }
        }

        int numModded = 0;
        debug.nscanned = 0;
        shared_ptr<Cursor> c =
            NamespaceDetailsTransient::getCursor( ns, patternOrig, BSONObj(), planPolicy );
        d = nsdetails(ns);
        nsdt = &NamespaceDetailsTransient::get(ns);
        bool autoDedup = c->autoDedup();

        if( c->ok() ) {
            set<DiskLoc> seenObjects;
            MatchDetails details;
            auto_ptr<ClientCursor> cc;
            do {

                if ( cc.get() == 0 &&
                     client.allowedToThrowPageFaultException() &&
                     ! c->currLoc().isNull() &&
                     ! c->currLoc().rec()->likelyInPhysicalMemory() ) {
                    throw PageFaultException( c->currLoc().rec() );
                }

                bool atomic = c->matcher() && c->matcher()->docMatcher().atomic();

                if ( ! atomic && debug.nscanned > 0 ) {
                    // we need to use a ClientCursor to yield
                    if ( cc.get() == 0 ) {
                        shared_ptr< Cursor > cPtr = c;
                        cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , cPtr , ns ) );
                    }

                    bool didYield;
                    if ( ! cc->yieldSometimes( ClientCursor::WillNeed, &didYield ) ) {
                        cc.release();
                        break;
                    }
                    if ( !c->ok() ) {
                        break;
                    }

                    if ( didYield ) {
                        d = nsdetails(ns);
                        nsdt = &NamespaceDetailsTransient::get(ns);
                    }

                } // end yielding block

                debug.nscanned++;

                if ( mods.get() && mods->hasDynamicArray() ) {
                    // The Cursor must have a Matcher to record an elemMatchKey.  But currently
                    // a modifier on a dynamic array field may be applied even if there is no
                    // elemMatchKey, so a matcher cannot be required.
                    //verify( c->matcher() );
                    details.requestElemMatchKey();
                }

                if ( !c->currentMatches( &details ) ) {
                    c->advance();

                    if ( debug.nscanned % 256 == 0 && ! atomic ) {
                        if ( cc.get() == 0 ) {
                            shared_ptr< Cursor > cPtr = c;
                            cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , cPtr , ns ) );
                        }
                        if ( ! cc->yield() ) {
                            cc.release();
                            // TODO should we assert or something?
                            break;
                        }
                        if ( !c->ok() ) {
                            break;
                        }
                        d = nsdetails(ns);
                        nsdt = &NamespaceDetailsTransient::get(ns);
                    }
                    continue;
                }

                Record* r = c->_current();
                DiskLoc loc = c->currLoc();

                if ( c->getsetdup( loc ) && autoDedup ) {
                    c->advance();
                    continue;
                }

                BSONObj js = BSONObj::make(r);

                BSONObj pattern = patternOrig;

                if ( logop ) {
                    BSONObjBuilder idPattern;
                    BSONElement id;
                    // NOTE: If the matching object lacks an id, we'll log
                    // with the original pattern.  This isn't replay-safe.
                    // It might make sense to suppress the log instead
                    // if there's no id.
                    if ( js.getObjectID( id ) ) {
                        idPattern.append( id );
                        pattern = idPattern.obj();
                    }
                    else {
                        uassert( 10157 ,  "multi-update requires all modified objects to have an _id" , ! multi );
                    }
                }

                /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
                    regular ones at the moment. */
                if ( isOperatorUpdate ) {

                    if ( multi ) {
                        // go to next record in case this one moves
                        c->advance();

                        // Update operations are deduped for cursors that implement their own
                        // deduplication.  In particular, some geo cursors are excluded.
                        if ( autoDedup ) {

                            if ( seenObjects.count( loc ) ) {
                                continue;
                            }

                            // SERVER-5198 Advance past the document to be modified, provided
                            // deduplication is enabled, but see SERVER-5725.
                            while( c->ok() && loc == c->currLoc() ) {
                                c->advance();
                            }
                        }
                    }

                    const BSONObj& onDisk = loc.obj();

                    ModSet* useMods = mods.get();
                    bool forceRewrite = false;

                    auto_ptr<ModSet> mymodset;
                    if ( details.hasElemMatchKey() && mods->hasDynamicArray() ) {
                        useMods = mods->fixDynamicArray( details.elemMatchKey() );
                        mymodset.reset( useMods );
                        forceRewrite = true;
                    }

                    auto_ptr<ModSetState> mss = useMods->prepare( onDisk );

                    bool willAdvanceCursor = multi && c->ok() && ( modsIsIndexed || ! mss->canApplyInPlace() );

                    if ( willAdvanceCursor ) {
                        if ( cc.get() ) {
                            cc->setDoingDeletes( true );
                        }
                        c->prepareToTouchEarlierIterate();
                    }

                    if ( modsIsIndexed <= 0 && mss->canApplyInPlace() ) {
                        mss->applyModsInPlace( true );// const_cast<BSONObj&>(onDisk) );

                        DEBUGUPDATE( "\t\t\t doing in place update" );
                        if ( profile && !multi )
                            debug.fastmod = true;

                        if ( modsIsIndexed ) {
                            seenObjects.insert( loc );
                        }

                        d->paddingFits();
                    }
                    else {
                        if ( rs )
                            rs->goingToDelete( onDisk );

                        BSONObj newObj = mss->createNewFromMods();
                        checkTooLarge(newObj);
                        DiskLoc newLoc = theDataFileMgr.updateRecord(ns,
                                                                     d,
                                                                     nsdt,
                                                                     r,
                                                                     loc,
                                                                     newObj.objdata(),
                                                                     newObj.objsize(),
                                                                     debug);

                        if ( newLoc != loc || modsIsIndexed ){
                            // log() << "Moved obj " << newLoc.obj()["_id"] << " from " << loc << " to " << newLoc << endl;
                            // object moved, need to make sure we don' get again
                            seenObjects.insert( newLoc );
                        }

                    }

                    if ( logop ) {
                        DEV verify( mods->size() );

                        if ( mss->haveArrayDepMod() ) {
                            BSONObjBuilder patternBuilder;
                            patternBuilder.appendElements( pattern );
                            mss->appendSizeSpecForArrayDepMods( patternBuilder );
                            pattern = patternBuilder.obj();
                        }

                        if ( forceRewrite || mss->needOpLogRewrite() ) {
                            DEBUGUPDATE( "\t rewrite update: " << mss->getOpLogRewrite() );
                            logOp("u", ns, mss->getOpLogRewrite() ,
                                  &pattern, 0, fromMigrate );
                        }
                        else {
                            logOp("u", ns, updateobj, &pattern, 0, fromMigrate );
                        }
                    }
                    numModded++;
                    if ( ! multi )
                        return UpdateResult( 1 , 1 , numModded , BSONObj() );
                    if ( willAdvanceCursor )
                        c->recoverFromTouchingEarlierIterate();

                    if ( debug.nscanned % 64 == 0 && ! atomic ) {
                        if ( cc.get() == 0 ) {
                            shared_ptr< Cursor > cPtr = c;
                            cc.reset( new ClientCursor( QueryOption_NoCursorTimeout , cPtr , ns ) );
                        }
                        if ( ! cc->yield() ) {
                            cc.release();
                            break;
                        }
                        if ( !c->ok() ) {
                            break;
                        }
                        d = nsdetails(ns);
                        nsdt = &NamespaceDetailsTransient::get(ns);
                    }

                    getDur().commitIfNeeded();

                    continue;
                }

                uassert( 10158 ,  "multi update only works with $ operators" , ! multi );

                BSONElementManipulator::lookForTimestamps( updateobj );
                checkNoMods( updateobj );
                theDataFileMgr.updateRecord(ns, d, nsdt, r, loc , updateobj.objdata(), updateobj.objsize(), debug, su);
                if ( logop ) {
                    DEV wassert( !su ); // super used doesn't get logged, this would be bad.
                    logOp("u", ns, updateobj, &pattern, 0, fromMigrate );
                }
                return UpdateResult( 1 , 0 , 1 , BSONObj() );
            } while ( c->ok() );
        } // endif

        if ( numModded )
            return UpdateResult( 1 , 1 , numModded , BSONObj() );

        if ( upsert ) {
            if ( updateobj.firstElementFieldName()[0] == '$' ) {
                // upsert of an $operation. build a default object
                BSONObj newObj = mods->createNewFromQuery( patternOrig );
                checkNoMods( newObj );
                debug.fastmodinsert = true;
                theDataFileMgr.insertWithObjMod(ns, newObj, su);
                if ( logop )
                    logOp( "i", ns, newObj, 0, 0, fromMigrate );

                return UpdateResult( 0 , 1 , 1 , newObj );
            }
            uassert( 10159 ,  "multi update only works with $ operators" , ! multi );
            checkNoMods( updateobj );
            debug.upsert = true;
            BSONObj no = updateobj;
            theDataFileMgr.insertWithObjMod(ns, no, su);
            if ( logop )
                logOp( "i", ns, no, 0, 0, fromMigrate );
            return UpdateResult( 0 , 0 , 1 , no );
        }

        return UpdateResult( 0 , isOperatorUpdate , 0 , BSONObj() );
    }

    UpdateResult updateObjects( const char* ns,
                                const BSONObj& updateobj,
                                const BSONObj& patternOrig,
                                bool upsert,
                                bool multi,
                                bool logop ,
                                OpDebug& debug,
                                bool fromMigrate,
                                const QueryPlanSelectionPolicy& planPolicy ) {

        uassert( 10155 , "cannot update reserved $ collection", strchr(ns, '$') == 0 );
        if ( strstr(ns, ".system.") ) {
            /* dm: it's very important that system.indexes is never updated as IndexDetails has pointers into it */
            uassert( 10156,
                     str::stream() << "cannot update system collection: " << ns << " q: " << patternOrig << " u: " << updateobj,
                     legalClientSystemNS( ns , true ) );
        }

        UpdateResult ur = _updateObjects(false, ns, updateobj, patternOrig,
                                         upsert, multi, logop,
                                         debug, 0, fromMigrate, planPolicy );
        debug.nupdated = ur.num;
        return ur;
    }

}  // namespace mongo
