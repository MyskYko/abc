/**CFile****************************************************************

  FileName    [extraUtilCspf.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [extra]

  Synopsis    [Minimization using permissible functions and the simple BDD package]

  Author      [Yukio Miyasaka]
  
  Affiliation [The University of Tokyo]

  Date        []

  Revision    []

***********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "extra.h"
#include "misc/vec/vec.h"
#include "aig/gia/gia.h"

ABC_NAMESPACE_IMPL_START

////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

typedef struct Abc_NandMan_ Abc_NandMan;
struct Abc_NandMan_ 
{
  int          nGiaObjs;
  int          nObjsAlloc;
  Vec_Int_t *  vPis;
  Vec_Int_t *  vPos;
  Vec_Int_t *  vObjs;
  Vec_Int_t ** pvFanins;
  Vec_Int_t ** pvFanouts;
  unsigned *   pBddFuncs;
  int *        pRank;
  char *       pMark;
  unsigned *   pGFuncs;
  Vec_Int_t ** pvCFuncs;
  
  int          nMem;
  int          nVerbose;
  
  Abc_BddMan * pBdd;
  Gia_Man_t *  pGia;
  Vec_Int_t *  vPiCkts;
  Vec_Int_t *  vPiIdxs;
  Vec_Ptr_t *  vvDcGias;

  int          fRm;
  int          nMspf;

  Vec_Int_t *  vOrgPis;
  Vec_Int_t *  vOrdering;
};

static inline int      Abc_BddNandConst0() { return 0; }  // = Gia_ObjId( pGia, Gia_ManConst0( pGia ) );

static inline int      Abc_BddNandObjIsPi( Abc_NandMan * p, int id ) { return (int)( p->pvFanins[id] == 0 ); }
static inline int      Abc_BddNandObjIsPo( Abc_NandMan * p, int id ) { return (int)( p->pvFanouts[id] == 0 ); }

static inline int      Abc_BddNandObjIsEmpty( Abc_NandMan * p, int id ) { return (int)( p->pvFanins[id] == 0 && p->pvFanouts[id] == 0 ); }
static inline int      Abc_BddNandObjIsDead( Abc_NandMan * p, int id ) { return (int)( Vec_IntSize( p->pvFanouts[id] ) == 0 ); }
static inline int      Abc_BddNandObjIsEmptyOrDead( Abc_NandMan * p, int id ) { return ( Abc_BddNandObjIsEmpty( p, id ) || Abc_BddNandObjIsDead( p, id ) ); }

static inline void     Abc_BddNandMemIncrease( Abc_NandMan * p ) {
  p->nMem++;
  if ( p->nMem >= 32 )
    {
      printf( "Error: Refresh failed\n" );
      abort();
    }
}

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////
	      
/**Function*************************************************************

  Synopsis    []

  Description [print circuit for debug]
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline void Abc_BddNandPrintNet( Abc_NandMan * p, char * fName )
{
  int i, j, id, idj; 
  FILE *fp;
  fp = fopen( fName, "w" );
  fprintf( fp, ".model test\n" );
  fprintf( fp, ".inputs" );
  Vec_IntForEachEntry( p->vPis, id, i )
    fprintf( fp, " pi%d", id - 1 );
  fprintf( fp, "\n.outputs" );
  Vec_IntForEachEntry( p->vPos, id, i )
    fprintf( fp, " po%d", i );
  fprintf( fp, "\n" );
  fprintf( fp, ".names const0\n0\n" );
  Vec_IntForEachEntry( p->vObjs, id, i )
    {
      if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
	continue;
      fprintf( fp,".names " );
      Vec_IntForEachEntry( p->pvFanins[id], idj, j )
	if ( idj == 0 )
	  fprintf( fp, "const0 " );	  
	else if ( idj <= Vec_IntSize( p->vPis ) ) // assuming (id of pi <= num pi)
	  fprintf( fp, "pi%d ", idj - 1 );
	else
	  fprintf( fp, "n%d ", idj );
      fprintf( fp, "n%d\n", id );
      for ( j = 0; j < Vec_IntSize( p->pvFanins[id] ); j++ ) 
	fprintf( fp, "1" );
      fprintf( fp, " 0\n" );
    }
  Vec_IntForEachEntry( p->vPos, id, i )
    {
      idj = Vec_IntEntry( p->pvFanins[id], 0 );
      fprintf( fp, ".names " );
      if ( idj == 0 )
	fprintf( fp, "const0 " );	  
      else if ( idj <= Vec_IntSize( p->vPis ) ) // assuming (id of pi <= num pi)
	fprintf( fp, "pi%d ", idj - 1 );
      else
	fprintf( fp, "n%d ", idj );
      fprintf( fp, "po%d\n", i );
      fprintf( fp, "1 1\n" );
    }
  fprintf( fp, ".end\n" );
  fflush( fp );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Abc_BddNandMarkDescendant_rec( Abc_NandMan * p, Vec_Int_t ** children, int id )
{
  int j, idj;
  Vec_IntForEachEntry( children[id], idj, j )
    if ( !p->pMark[idj] && children[idj] ) // idj is not marked and not leaf
      {
	p->pMark[idj] = 1;
	Abc_BddNandMarkDescendant_rec( p, children, idj );
      }
}
static inline void Abc_BddNandMarkClear( Abc_NandMan * p )
{
  ABC_FREE( p->pMark );
  p->pMark = ABC_CALLOC( char, p->nObjsAlloc );
}
void Abc_BddNandDescendantList_rec( Abc_NandMan * p, Vec_Int_t ** children, Vec_Int_t * list, int id )
{
  int j, idj;
  Vec_IntForEachEntry( children[id], idj, j )
    if ( !(p->pMark[idj] >> 1) && children[idj] ) // idj is not marked and not leaf
      {
	p->pMark[idj] += 2;
	Vec_IntPush( list, idj );
	Abc_BddNandDescendantList_rec( p, children, list, idj );
      }
}
static inline void Abc_BddNandSortList( Abc_NandMan * p, Vec_Int_t * list )
{
  int i, id, index;
  Vec_Int_t * listOld;
  listOld = Vec_IntDup( list );
  Vec_IntClear( list );
  Vec_IntForEachEntry( p->vObjs, id, i )
    {
      index = Vec_IntFind( listOld, id );
      if ( index != -1 )
	{
	  Vec_IntDrop( listOld, index );      
	  Vec_IntPush( list, id );
	  if ( !Vec_IntSize( listOld ) )
	    break;
	}
    }
  Vec_IntFree( listOld );
}

static inline void Abc_BddNandDescendantSortedList( Abc_NandMan * p, Vec_Int_t ** children, Vec_Int_t * list, int id )
{
  int j, idj;
  Abc_BddNandDescendantList_rec( p, children, list, id );
  Vec_IntForEachEntry( list, idj, j )
    p->pMark[idj] -= 2;
  Abc_BddNandSortList( p, list );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Abc_BddNandObjEntry( Abc_NandMan * p, int id )
{
  int j, idj, index, index_;
  index = Vec_IntSize( p->vObjs );
  Vec_IntForEachEntry( p->pvFanouts[id], idj, j )
    {
      index_ = Vec_IntFind( p->vObjs, idj );
      if ( index_ != -1 && index_ < index )
	index = index_;
    }
  Vec_IntInsert( p->vObjs, index, id );
  Vec_IntForEachEntry( p->pvFanins[id], idj, j )
    {
      index_ = Vec_IntFind( p->vObjs, idj );
      if ( index_ > index )
	{
	  Vec_IntDrop( p->vObjs, index_ );
	  Abc_BddNandObjEntry( p, idj );
	  index = Vec_IntFind( p->vObjs, id );
	}
    }
}
static inline void Abc_BddNandConnect( Abc_NandMan * p, int fanin, int fanout, int fSort )
{ // uniqueness of conenction must be confirmed beforehand
  int index_fanin, index_fanout;
  Vec_IntPush( p->pvFanins[fanout], fanin );    
  Vec_IntPush( p->pvFanouts[fanin], fanout );
  if ( fSort )
    {
      index_fanin = Vec_IntFind( p->vObjs, fanin );
      index_fanout = Vec_IntFind( p->vObjs, fanout );
      if ( index_fanout != -1 && index_fanout < index_fanin )
	{ // Omit the case fanout is not in vObjs for G3, and sort.
	  Vec_IntDrop( p->vObjs, index_fanin );
	  Abc_BddNandObjEntry( p, fanin );
	}
    }
}
static inline void Abc_BddNandDisconnect( Abc_NandMan * p, int fanin, int fanout )
{
  Vec_IntRemove( p->pvFanins[fanout], fanin );    
  Vec_IntRemove( p->pvFanouts[fanin], fanout );
}
static inline void Abc_BddNandRemoveNode( Abc_NandMan * p, int id )
{
  int j, idj;
  Vec_IntForEachEntry( p->pvFanins[id], idj, j )
    Vec_IntRemove( p->pvFanouts[idj], id );
  Vec_IntForEachEntry( p->pvFanouts[id], idj, j )
    Vec_IntRemove( p->pvFanins[idj], id );
  Vec_IntFree( p->pvFanins[id] );
  Vec_IntFree( p->pvFanouts[id] );
  p->pvFanins[id] = 0;
  p->pvFanouts[id] = 0;
  Vec_IntRemove( p->vObjs, id );
}
static inline int Abc_BddNandCountWire( Abc_NandMan * p )
{
  int i, id, count;
  count = 0;
  Vec_IntForEachEntry( p->vObjs, id, i )
    count += Vec_IntSize( p->pvFanins[id] );
  return count;
}

/**Function*************************************************************
   
  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline void Abc_BddNandGia2Net( Abc_NandMan * p )
{
  Gia_Obj_t * pObj, * pObj0, * pObj1;
  int i, id, idj, id0, id1;
  // constant
  id = Abc_BddNandConst0();
  p->pBddFuncs[id] = Abc_BddLitConst0();
  p->pvFanins[id] = 0;
  p->pvFanouts[id] = Vec_IntAlloc( 1 );
  idj = id + p->nGiaObjs;
  p->pvFanins[idj] = Vec_IntAlloc( 1 );
  p->pvFanouts[idj] = Vec_IntAlloc( 1 );
  Vec_IntPush( p->vObjs, idj );
  Abc_BddNandConnect( p, id, idj, 0 );
  // pi
  Gia_ManForEachCi( p->pGia, pObj, i )
    {
      id = Gia_ObjId( p->pGia, pObj );
      p->pBddFuncs[id] = Abc_BddLitIthVar( i );
      p->pvFanins[id] = 0;
      p->pvFanouts[id] = Vec_IntAlloc( 1 );
      Vec_IntPush( p->vPis, id );
      idj = id + p->nGiaObjs;
      p->pvFanins[idj] = Vec_IntAlloc( 1 );
      p->pvFanouts[idj] = Vec_IntAlloc( 1 );
      Vec_IntPush( p->vObjs, idj );
      Abc_BddNandConnect( p, id, idj, 0 );
    }
  // gate
  Gia_ManForEachAnd( p->pGia, pObj, i )
    {
      id = Gia_ObjId( p->pGia, pObj );
      p->pvFanins[id] = Vec_IntAlloc( 1 );
      p->pvFanouts[id] = Vec_IntAlloc( 1 );
      pObj0 = Gia_ObjFanin0( pObj );
      pObj1 = Gia_ObjFanin1( pObj );
      id0 = Gia_ObjId( p->pGia, pObj0 );
      id1 = Gia_ObjId( p->pGia, pObj1 );
      if ( (  Gia_ObjIsCi( pObj0 ) &&  Gia_ObjFaninC0( pObj ) ) ||
	   ( !Gia_ObjIsCi( pObj0 ) && !Gia_ObjFaninC0( pObj ) ) )
	id0 += p->nGiaObjs;
      if ( (  Gia_ObjIsCi( pObj1 ) &&  Gia_ObjFaninC1( pObj ) ) ||
	   ( !Gia_ObjIsCi( pObj1 ) && !Gia_ObjFaninC1( pObj ) ) )  
	id1 += p->nGiaObjs;
      Abc_BddNandConnect( p, id0, id, 0 );
      Abc_BddNandConnect( p, id1, id, 0 );
      Vec_IntPush( p->vObjs, id );
      // create inverter
      idj = id + p->nGiaObjs;
      p->pvFanins[idj] = Vec_IntAlloc( 1 );
      p->pvFanouts[idj] = Vec_IntAlloc( 1 );
      Abc_BddNandConnect( p, id, idj, 0 );
      Vec_IntPush( p->vObjs, idj );    
    }
  // po
  Gia_ManForEachCo( p->pGia, pObj, i )
    {
      id = Gia_ObjId( p->pGia, pObj );
      p->pvFanins[id] = Vec_IntAlloc( 1 );
      p->pvFanouts[id] = 0;
      pObj0 = Gia_ObjFanin0( pObj );
      id0 = Gia_ObjId( p->pGia, pObj0 );
      if ( (  ( id0 == Abc_BddNandConst0() || Gia_ObjIsCi( pObj0 ) ) &&  Gia_ObjFaninC0( pObj ) ) ||
	   ( !( id0 == Abc_BddNandConst0() || Gia_ObjIsCi( pObj0 ) ) && !Gia_ObjFaninC0( pObj ) ) )
	id0 += p->nGiaObjs;
      Abc_BddNandConnect( p, id0, id, 0 );
      Vec_IntPush( p->vPos, id );
    }
  // remove redundant nodes
  Vec_IntForEachEntryReverse( p->vObjs, id, i )
    if ( !Vec_IntSize( p->pvFanouts[id] ) )
      Abc_BddNandRemoveNode( p, id );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline Abc_NandMan * Abc_BddNandManAlloc( Gia_Man_t * pGia, int nMem, int fRm, int nMspf, int nVerbose )
{
  int i;
  Abc_NandMan * p;
  p = ABC_CALLOC( Abc_NandMan, 1 );
  p->nGiaObjs = pGia->nObjs;
  p->nObjsAlloc = pGia->nObjs + pGia->nObjs;
  p->vPis = Vec_IntAlloc( Gia_ManCiNum( pGia ) );
  p->vPos = Vec_IntAlloc( Gia_ManCoNum( pGia ) );
  p->vObjs = Vec_IntAlloc( 1 );
  p->pvFanins  = ABC_CALLOC( Vec_Int_t *, p->nObjsAlloc );
  p->pvFanouts = ABC_CALLOC( Vec_Int_t *, p->nObjsAlloc );
  p->pBddFuncs = ABC_CALLOC( unsigned   , p->nObjsAlloc );
  p->pRank     = ABC_CALLOC( int        , p->nObjsAlloc );
  p->pMark     = ABC_CALLOC( char       , p->nObjsAlloc );
  p->pGFuncs   = ABC_CALLOC( unsigned   , p->nObjsAlloc );
  p->pvCFuncs  = ABC_CALLOC( Vec_Int_t *, p->nObjsAlloc );
  if ( !p->pvFanins || !p->pvFanouts || !p->pBddFuncs || !p->pRank || !p->pMark || !p->pGFuncs || !p->pvCFuncs )
    {
      printf( "Error: Allocation failed\n" );
      abort();
    }
  p->nMem = nMem;
  p->fRm = fRm;
  p->nMspf = nMspf;
  p->nVerbose = nVerbose;
  p->vOrgPis = NULL;
  p->vOrdering = NULL;
  p->pGia = pGia;
  p->vPiCkts = Vec_IntAlloc( 1 );
  p->vPiIdxs = Vec_IntAlloc( 1 );
  p->vvDcGias = Vec_PtrAlloc( 1 );
  for ( i = 0; i < Gia_ManCoNum( pGia ); i++ )
    Vec_PtrPush( p->vvDcGias, Vec_PtrAlloc( 1 ) );
  Abc_BddNandGia2Net( p );
  return p;
}
static inline void Abc_BddNandManFree( Abc_NandMan * p )
{
  int i, j;
  Vec_Ptr_t * vDcGias;
  Gia_Man_t * pGia;
  Vec_IntFree( p->vPis );
  Vec_IntFree( p->vPos );
  Vec_IntFree( p->vObjs );
  Vec_IntFree( p->vPiCkts );
  Vec_IntFree( p->vPiIdxs );
  if ( p->vOrgPis )
    Vec_IntFree( p->vOrgPis );
  if ( p->vOrdering )
    Vec_IntFree( p->vOrdering );
  ABC_FREE( p->pBddFuncs );
  ABC_FREE( p->pRank );
  ABC_FREE( p->pMark );
  ABC_FREE( p->pGFuncs );
  for ( i = 0; i < p->nObjsAlloc; i++ )
    {
      if ( p->pvFanins[i] )
	Vec_IntFree( p->pvFanins[i] );
      if ( p->pvFanouts[i] )
	Vec_IntFree( p->pvFanouts[i] );
      if ( p->pvCFuncs[i] )
	Vec_IntFree( p->pvCFuncs[i] );
    }
  ABC_FREE( p->pvFanins );
  ABC_FREE( p->pvFanouts );
  ABC_FREE( p->pvCFuncs );
  Gia_ManStop( p->pGia );
  Vec_PtrForEachEntry( Vec_Ptr_t *, p->vvDcGias, vDcGias, i )
    Vec_PtrForEachEntry( Gia_Man_t *, vDcGias, pGia, j )
      Gia_ManStop( pGia );
  Vec_PtrFree( p->vvDcGias );
  Abc_BddManFree( p->pBdd );
  ABC_FREE( p );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline Gia_Man_t * Abc_BddNandGiaExpand( Gia_Man_t * pGia, Vec_Int_t * vPis, Vec_Int_t * vPos )
{
  int i, id;
  Gia_Obj_t * pObj;
  Gia_Man_t * pNew, * pTmp;
  Gia_ManFillValue( pGia );
  pNew = Gia_ManStart( Gia_ManObjNum( pGia ) );
  pNew->pName = Abc_UtilStrsav( pGia->pName );
  pNew->pSpec = Abc_UtilStrsav( pGia->pSpec );
  Gia_ManHashAlloc( pNew );
  Gia_ManConst0( pGia )->Value = 0;
  Gia_ManForEachCi( pGia, pObj, i )
    pObj->Value = Gia_ManAppendCi( pNew );
  if ( vPis )
    Vec_IntForEachEntry( vPis, id, i )
      Gia_ManObj( pGia, id )->Value = Gia_ManAppendCi( pNew );
  Gia_ManForEachAnd( pGia, pObj, i )
    if ( !vPis || Vec_IntFind( vPis, Gia_ObjId( pGia, pObj ) ) == -1 )
      pObj->Value = Gia_ManHashAnd( pNew, Gia_ObjFanin0Copy( pObj ), Gia_ObjFanin1Copy( pObj ) );
  Gia_ManForEachPo( pGia, pObj, i )
    pObj->Value = Gia_ManAppendCo( pNew, Gia_ObjFanin0Copy( pObj ) );
  if ( vPos )
    Vec_IntForEachEntry( vPos, id, i )
      Gia_ManAppendCo( pNew, Gia_ManObj( pGia, id )->Value );
  Gia_ManHashStop( pNew );
  pNew = Gia_ManCleanup( pTmp = pNew );  
  Gia_ManStop( pTmp );
  return pNew;
}
static inline void Abc_BddNandSetPoInfo( Gia_Man_t * pGia, Vec_Ptr_t * vNets, Vec_Ptr_t * vvPis, Vec_Ptr_t * vvPos, Vec_Int_t * vExternalPos, Vec_Int_t * vPoCkts, Vec_Int_t * vPoIdxs, int fDc )
{
  int i, j, k, id;
  Gia_Obj_t * pObj;
  Vec_Int_t * vId, * vCkts, * vIdxs, * vPis, * vPos;
  Gia_Man_t * pDc, * pTmp;
  Abc_NandMan * p;
  vId = Vec_IntAlloc( 1 );
  vCkts = Vec_IntAlloc( 1 );
  vIdxs = Vec_IntAlloc( 1 );
  // const0
  id = Abc_BddNandConst0();
  Vec_IntPush( vId, id );
  Vec_IntPush( vCkts, -2 );
  Vec_IntPush( vIdxs, 0 );
  // Generate Po list
  Gia_ManForEachCi( pGia, pObj, i )
    {
      id = Gia_ObjId( pGia, pObj );
      Vec_IntPush( vId, id );
      Vec_IntPush( vCkts, -1 );
      Vec_IntPush( vIdxs, i );
    }
  Vec_PtrForEachEntry( Vec_Int_t *, vvPos, vPos, j )
    Vec_IntForEachEntry( vPos, id, i )
    {
      Vec_IntPush( vId, id );
      Vec_IntPush( vCkts, j );
      Vec_IntPush( vIdxs, i );
    }
  // Assign Pi info
  Vec_PtrForEachEntry( Abc_NandMan *, vNets, p, j )
    {
      vPis = Vec_PtrEntry( vvPis, j );
      Vec_IntForEachEntry( vPis, id, i )
	{
	  k = Vec_IntFind( vId, id );
	  Vec_IntPush( p->vPiCkts, Vec_IntEntry( vCkts, k ) );
	  Vec_IntPush( p->vPiIdxs, Vec_IntEntry( vIdxs, k ) );
	}
    }
  Vec_IntForEachEntry( vExternalPos, id, i )
    {
      k = Vec_IntFind( vId, id );
      Vec_IntPush( vPoCkts, Vec_IntEntry( vCkts, k ) );
      Vec_IntPush( vPoIdxs, Vec_IntEntry( vIdxs, k ) );
      if ( Vec_IntEntry( vCkts, k ) < 0 )
	continue;
      // set external don't care
      p = Vec_PtrEntry( vNets, Vec_IntEntry( vCkts, k ) );
      if ( fDc )
	{
	  // from latter half outputs 
	  pDc = Abc_BddNandGiaExpand( pGia, p->vOrgPis, NULL );
	  j = i + Vec_IntSize( vExternalPos );
	  pTmp = pDc;
	  pDc = Gia_ManDupCones( pDc, &j, 1, 0 );
	  Gia_ManStop( pTmp );
	  for ( j = 0; j < Gia_ManCiNum( pGia ); j++ )
	    {
	      pTmp = pDc;
	      pDc = Gia_ManDupUniv( pDc, j );
	      Gia_ManStop( pTmp );
	    }
	  pTmp = pDc;
	  pDc = Gia_ManDupLastPis( pDc, Vec_IntSize( p->vOrgPis ) );
	  Gia_ManStop( pTmp );
	}
      else
	{
	  // const-0
	  pDc = Gia_ManStart( Vec_IntSize( p->vPis ) );
	  Vec_IntForEachEntry( p->vPis, id, j )
	    Gia_ManAppendCi( pDc );
	  Gia_ManAppendCo( pDc, Gia_ManConst0Lit() );
	}
      Vec_PtrPush( Vec_PtrEntry( p->vvDcGias, Vec_IntEntry( vIdxs, k ) ), pDc );
    }
  Vec_IntFree( vId );
  Vec_IntFree( vCkts );
  Vec_IntFree( vIdxs );
}
static inline int Abc_BddNandCountNewFanins( Gia_Man_t * pGia, Gia_Obj_t * pObj, int * pParts, int part )
{
  int id0, id1;
  Gia_Obj_t * pObj0, * pObj1;  
  pObj0 = Gia_ObjFanin0( pObj );
  id0 = Gia_ObjId( pGia, pObj0 );
  pObj1 = Gia_ObjFanin1( pObj );
  id1 = Gia_ObjId( pGia, pObj1 );
  return (int)(pParts[id0] != part) + (int)(pParts[id1] != part);
}
static inline int Abc_BddNandGia2NetsNext( Gia_Man_t * pGia, Vec_Int_t * v,  int * pFanouts, int * pParts, int part, int num, int drop )
{
  int i, id;
  Gia_Obj_t * pObj;
  Vec_IntForEachEntry( v, id, i )
    {
      pObj = Gia_ManObj( pGia, id );
      if ( !Gia_ObjIsConst0( pObj ) &&
	   !Gia_ObjIsCi( pObj ) &&
	   !pFanouts[id] &&
	   Abc_BddNandCountNewFanins( pGia, pObj, pParts, part ) == num )
	{
	  if ( drop )
	    Vec_IntDrop( v, i );
	  return id;
	}
    }
  return Abc_BddNandConst0();
}
static inline void recursive_dec( Gia_Man_t * pGia, int * pFanouts, Gia_Obj_t * pObj )
{
  int id;
  if ( Gia_ObjIsConst0( pObj ) || Gia_ObjIsCi( pObj ) )
    return;
  id = Gia_ObjId( pGia, pObj );
  pFanouts[id]--;
  assert( pFanouts[id] >= 0 );
  if ( pFanouts[id] )
    return;
  recursive_dec( pGia, pFanouts, Gia_ObjFanin0( pObj ) );
  recursive_dec( pGia, pFanouts, Gia_ObjFanin1( pObj ) );
}
static inline void Abc_BddNandGia2Nets( Gia_Man_t * pOld, Vec_Ptr_t * vNets, Vec_Int_t * vPoCkts, Vec_Int_t * vPoIdxs, Vec_Int_t * vExternalCs, int nMem, int fRm, int fDc, int nWindowSize, int nMspf, int nVerbose )
{
  int i, id, lit, newId, part, nPos;
  int * pFanouts, * pParts;
  Vec_Int_t * vPis, * vPos, * vTempPos, * vNodes, * vExternalPos, * vCands;
  Vec_Ptr_t * vvPis, * vvPos;
  Gia_Man_t * pGia, * pNew;
  Gia_Obj_t * pObj, * pObj0, * pObj1;
  Abc_NandMan * p;
  pGia = Gia_ManDup ( pOld );
  pFanouts = ABC_CALLOC( int, pGia->nObjs );
  pParts = ABC_CALLOC( int, pGia->nObjs );
  if ( !pFanouts || !pParts )
    {
      printf( "Error: Allocation failed\n" );
      abort();
    }
  Abc_BddGiaCountFanout( pGia, pFanouts );
  vExternalPos = Vec_IntAlloc( 1 );
  vCands = Vec_IntAlloc( 1 );
  vvPis = Vec_PtrAlloc( 1 );
  vvPos = Vec_PtrAlloc( 1 );
  vTempPos = Vec_IntAlloc( 1 );
  vNodes = Vec_IntAlloc( 1 );
  // get po information
  if ( fDc )
    nPos = Gia_ManCoNum( pGia ) / 2;
  else
    nPos = Gia_ManCoNum( pGia );
  Gia_ManForEachCo( pGia, pObj, i )
    if ( i < nPos )
      {
	Vec_IntPush( vExternalCs, Gia_ObjFaninC0( pObj ) );
	pObj = Gia_ObjFanin0( pObj );
	id = Gia_ObjId( pGia, pObj );
	Vec_IntPush( vExternalPos, id );
	if ( Gia_ObjIsConst0( pObj ) || Gia_ObjIsCi( pObj ) )
	  continue;
	pFanouts[id]--;
	Vec_IntPushUnique( vCands, id );
      }
    else
      {
	pObj = Gia_ObjFanin0( pObj );
	recursive_dec( pGia, pFanouts, pObj );
      }
  // Partition
  part = 0;
  while ( 1 )
    {
      part++;
      vPis = Vec_IntAlloc( 1 );
      vPos = Vec_IntAlloc( 1 );
      Vec_IntClear( vTempPos );
      Vec_IntClear( vNodes );
      while ( 1 )
	{
	  newId = Abc_BddNandGia2NetsNext( pGia, vPis, pFanouts, pParts, part, 0, 1 );
	  if ( newId == Abc_BddNandConst0() )
	    newId = Abc_BddNandGia2NetsNext( pGia, vPis, pFanouts, pParts, part, 1, 1 );
	  if ( newId == Abc_BddNandConst0() )
	    newId = Abc_BddNandGia2NetsNext( pGia, vPis, pFanouts, pParts, part, 2, 1 );
	  if ( newId == Abc_BddNandConst0() )
	    newId = Abc_BddNandGia2NetsNext( pGia, vCands, pFanouts, pParts, part, 0, 0 );
	  if ( newId == Abc_BddNandConst0() )
	    newId = Abc_BddNandGia2NetsNext( pGia, vCands, pFanouts, pParts, part, 1, 0 );
	  if ( newId == Abc_BddNandConst0() )
	    newId = Abc_BddNandGia2NetsNext( pGia, vCands, pFanouts, pParts, part, 2, 0 );
	  if ( newId == Abc_BddNandConst0() )
	    break;
	  i = Vec_IntFind( vCands, newId );
	  if ( i != -1 )
	    {
	      Vec_IntDrop( vCands, i );
	      Vec_IntPush( vPos, newId );
	      pObj = Gia_ManObj( pGia, newId );
	      lit = Gia_Obj2Lit( pGia, pObj );
	      lit = Gia_ManAppendCo( pGia, lit );
	      pObj = Gia_Lit2Obj( pGia, lit );
	      id = Gia_ObjId( pGia, pObj );
	      Vec_IntPush( vTempPos, id );
	    }
	  Vec_IntPush( vNodes, newId );
	  pObj = Gia_ManObj( pGia, newId );
	  pObj0 = Gia_ObjFanin0( pObj );
	  id = Gia_ObjId( pGia, pObj0 );
	  if ( !Gia_ObjIsCi( pObj0 ) )
	    pFanouts[id]--;
	  assert( pFanouts[id] >= 0 );
	  if ( pParts[id] != part )
	    {
	      pParts[id] = part;
	      Vec_IntPush( vPis, id );
	    }
	  pObj1 = Gia_ObjFanin1( pObj );
	  id = Gia_ObjId( pGia, pObj1 );
	  if ( !Gia_ObjIsCi( pObj1 ) )
	    pFanouts[id]--;
	  assert( pFanouts[id] >= 0 );
	  if ( pParts[id] != part )
	    {
	      pParts[id] = part;
	      Vec_IntPush( vPis, id );
	    }
	  if ( Vec_IntSize( vNodes ) >= nWindowSize )
	    break;
	}
      Vec_IntSort( vNodes, 0 );
      pNew = Gia_ManDupFromVecs( pGia, vPis, vNodes, vTempPos, 0 );
      p = Abc_BddNandManAlloc( pNew, nMem, fRm, nMspf, nVerbose );
      Vec_PtrPush( vNets, p );
      p->vOrgPis = vPis;
      Vec_PtrPush( vvPis, vPis );
      Vec_PtrPush( vvPos, vPos );
      Vec_IntForEachEntry( vPis, id, i )
	Vec_IntPushUnique( vCands, id );
      if ( newId == Abc_BddNandConst0() )
	break;
    }
  // create map to inputs
  Abc_BddNandSetPoInfo( pGia, vNets, vvPis, vvPos, vExternalPos, vPoCkts, vPoIdxs, fDc );
  Gia_ManStop( pGia );
  ABC_FREE( pFanouts );
  ABC_FREE( pParts );
  Vec_IntFree( vExternalPos );
  Vec_IntFree( vCands );
  Vec_IntFree( vTempPos );
  Vec_IntFree( vNodes );
  //  Vec_PtrForEachEntry( Vec_Int_t *, vvPis, vPis, i )
  //    Vec_IntFree( vPis );
  Vec_PtrFree( vvPis );
  Vec_PtrForEachEntry( Vec_Int_t *, vvPos, vPos, i )
    Vec_IntFree( vPos );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Abc_BddNandBuild( Abc_NandMan * p, int id )
{
  int j, idj;
  unsigned x;
  x = Abc_BddLitConst1();
  Vec_IntForEachEntry( p->pvFanins[id], idj, j )
    x = Abc_BddAnd( p->pBdd, x, p->pBddFuncs[idj] );
  if ( Abc_BddLitIsInvalid( x ) )
    return -1;
  p->pBddFuncs[id] = Abc_BddLitNot( x );
  return 0;
}
static inline int Abc_BddNandBuildAll( Abc_NandMan * p )
{
  int i, id;
  Vec_IntForEachEntry( p->vObjs, id, i )
    if ( Abc_BddNandBuild( p, id ) )
      return -1;
  return 0;
}
static inline int Abc_BddNandBuildFanoutCone( Abc_NandMan * p, int startId )
{ // including startId itself
  int i, id;
  Vec_Int_t * targets = Vec_IntAlloc( 1 );
  p->pMark[startId] += 2;
  Vec_IntPush( targets, startId );
  Abc_BddNandDescendantSortedList( p, p->pvFanouts, targets, startId );
  Vec_IntForEachEntry( targets, id, i )
    if ( Abc_BddNandBuild( p, id ) )
      {
	Vec_IntFree( targets );
	return -1;
      }
  Vec_IntFree( targets );
  return 0;
}
static inline int Abc_BddNandCheck( Abc_NandMan * p )
{
  int i, j, id, idj;
  unsigned x;
  Vec_IntForEachEntry( p->vObjs, id, i )
    {
      x = Abc_BddLitConst1();
      Vec_IntForEachEntry( p->pvFanins[id], idj, j )
	x = Abc_BddAnd( p->pBdd, x, p->pBddFuncs[idj] );
      if ( !Abc_BddLitIsEq( p->pBddFuncs[id], Abc_BddLitNot( x ) ) )
	{
	  printf( "Eq-check faild: different at %d %10u %10u\n", id, p->pBddFuncs[id], Abc_BddLitNot( x ) );
	  return -1;
	}
    }
  return 0;
}
static inline int Abc_BddNandBuildInverted( Abc_NandMan * p, int id, int startId )
{
  int j, idj;
  unsigned x;
  x = Abc_BddLitConst1();
  Vec_IntForEachEntry( p->pvFanins[id], idj, j )
    if ( idj == startId )
      x = Abc_BddAnd( p->pBdd, x, Abc_BddLitNot( p->pBddFuncs[idj] ) );
    else
      x = Abc_BddAnd( p->pBdd, x, p->pBddFuncs[idj] );
  if ( Abc_BddLitIsInvalid( x ) )
    return -1;
  p->pBddFuncs[id] = Abc_BddLitNot( x );
  return 0;
}
static inline int Abc_BddNandBuildFanoutConeInverted( Abc_NandMan * p, int startId )
{ // insert inverters between startId and the fanout node
  int i, id;
  Vec_Int_t * targets;
  targets = Vec_IntAlloc( 1 );
  Abc_BddNandDescendantSortedList( p, p->pvFanouts, targets, startId );
  Vec_IntForEachEntry( targets, id, i )
    if ( Abc_BddNandBuildInverted( p, id, startId ) )
      {
	Vec_IntFree( targets );
	return -1;
      }
  Vec_IntFree( targets );
  return 0;
}

/**Function*************************************************************
   
  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Abc_BddNandDc( Abc_NandMan * p )
{
  int i, j;
  unsigned x;
  Gia_Obj_t * pObj;
  Gia_Man_t * pGia;
  Vec_Ptr_t * vDcGias;
  Vec_PtrForEachEntry( Vec_Ptr_t *, p->vvDcGias, vDcGias, i )
    {
      if ( !Vec_PtrSize( vDcGias ) )
	continue;
      x = Abc_BddLitConst1();
      Vec_PtrForEachEntry( Gia_Man_t *, vDcGias, pGia, j )
	{
	  if ( Abc_BddGia( pGia, p->pBdd ) )
	    return -1;
	  pObj = Gia_ManCo( pGia, 0 );
	  x = Abc_BddAnd( p->pBdd, x, pObj->Value );
	  if ( Abc_BddLitIsInvalid( x ) )
	    return -1;
	  if ( Abc_BddLitIsConst0( x ) )
	    break;
	}
      p->pGFuncs[Vec_IntEntry( p->vPos, i )] = x;
    }
  return 0;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Abc_BddNandGFunc( Abc_NandMan * p, int id )
{
  int j, idj, index;
  unsigned x;
  p->pGFuncs[id] = Abc_BddLitConst1();
  Vec_IntForEachEntry( p->pvFanouts[id], idj, j )
    {
      if ( Abc_BddNandObjIsPo( p, idj ) )
	p->pGFuncs[id] = Abc_BddAnd( p->pBdd, p->pGFuncs[id], p->pGFuncs[idj] );
        // pGFuncs[idj] will be 0 unless external don't care of po is given.
      else
	{
	  index = Vec_IntFind( p->pvFanins[idj], id );
	  x = Vec_IntEntry( p->pvCFuncs[idj], index );
	  p->pGFuncs[id] = Abc_BddAnd( p->pBdd, p->pGFuncs[id], x );
	}
    }
  if ( Abc_BddLitIsInvalid( p->pGFuncs[id] ) )
    return -1;
  return 0;
}
/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline void Abc_BddNandRank( Abc_NandMan * p, int id )
{
  if ( Abc_BddNandObjIsPi( p, id ) )
    {
      p->pRank[id] = 1 << 30; // assume this is the max
      return;
    }
  p->pRank[id] = Vec_IntSize( p->pvFanouts[id] );
  assert( p->pRank[id] >= 0 );
}
static inline void Abc_BddNandRankAll( Abc_NandMan * p )
{
  int i, id;
  Vec_IntForEachEntry( p->vPis, id, i )
    Abc_BddNandRank( p, id );
  Vec_IntForEachEntry( p->vObjs, id, i )
    Abc_BddNandRank( p, id );
}
static inline void Abc_BddNandSortFanin( Abc_NandMan * p, int id )
{
  int j, k, idj, idk, best_j, best_idj;
  Vec_IntForEachEntry( p->pvFanins[id], idj, j )
    {
      best_j = j;
      best_idj = idj;
      Vec_IntForEachEntryStart( p->pvFanins[id], idk, k, j + 1 )
	if ( p->pRank[idj] > p->pRank[idk] )
	  {
	    best_j = k;
	    best_idj = idk;
	  }
      Vec_IntWriteEntry( p->pvFanins[id], j, best_idj );
      Vec_IntWriteEntry( p->pvFanins[id], best_j, idj );
    }
}
static inline void Abc_BddNandSortFaninAll( Abc_NandMan * p )
{
  int i, id;
  Vec_IntForEachEntry( p->vObjs, id, i )
    Abc_BddNandSortFanin( p, id );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Abc_BddNandRemoveRedundantFanin( Abc_NandMan * p, int id )
{
  int j, k, idj, idk;
  unsigned x;
  Vec_IntForEachEntry( p->pvFanins[id], idj, j )
    {
      x = Abc_BddLitConst1();
      Vec_IntForEachEntry( p->pvFanins[id], idk, k )
	if ( k != j )
	  x = Abc_BddAnd( p->pBdd, x, p->pBddFuncs[idk] );
      x = Abc_BddOr( p->pBdd, Abc_BddLitNot( x ), p->pGFuncs[id] );
      x = Abc_BddOr( p->pBdd, x, p->pBddFuncs[idj] );
      if ( Abc_BddLitIsInvalid( x ) )
	return -1;
      if ( Abc_BddLitIsConst1( x ) )
	{
	  Abc_BddNandDisconnect( p, idj, id );
	  if ( !Vec_IntSize( p->pvFanins[id] ) )
	    {
	      Vec_IntForEachEntry( p->pvFanouts[id], idk, k )
		if ( Vec_IntFind( p->pvFanins[idk], Abc_BddNandConst0() ) == -1 )
		  Abc_BddNandConnect( p, Abc_BddNandConst0(), idk, 0 );
	      Abc_BddNandRemoveNode( p, id );
	      return 0;
	    }
	  j--;
	  continue;
	}
    }
  return 0;
}
static inline int Abc_BddNandCFuncCspf( Abc_NandMan * p, int id )
{
  int j, k, idj, idk;
  unsigned x, y;
  if ( p->fRm )
    if ( Abc_BddNandRemoveRedundantFanin( p, id ) )
      return -1;
  if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
    return 0;
  if ( !p->pvCFuncs[id] )
    p->pvCFuncs[id] = Vec_IntAlloc( Vec_IntSize( p->pvFanins[id] ) );
  Vec_IntClear( p->pvCFuncs[id] );
  Vec_IntForEachEntry( p->pvFanins[id], idj, j )
    {
      x = Abc_BddLitConst1();
      Vec_IntForEachEntryStart( p->pvFanins[id], idk, k, j + 1 )
	x = Abc_BddAnd( p->pBdd, x, p->pBddFuncs[idk] );
      x = Abc_BddOr( p->pBdd, Abc_BddLitNot( x ), p->pGFuncs[id] );
      y = Abc_BddAnd( p->pBdd, p->pBddFuncs[id], p->pBddFuncs[idj] );
      x = Abc_BddOr( p->pBdd, x, y );
      y = Abc_BddOr( p->pBdd, x, p->pBddFuncs[idj] );
      if ( Abc_BddLitIsInvalid( y ) )
	return -1;
      if ( Abc_BddLitIsConst1( y ) )
	{
	  Abc_BddNandDisconnect( p, idj, id );
	  if ( !Vec_IntSize( p->pvFanins[id] ) )
	    {
	      Vec_IntForEachEntry( p->pvFanouts[id], idk, k )
		if ( Vec_IntFind( p->pvFanins[idk], Abc_BddNandConst0() ) == -1 )
		  Abc_BddNandConnect( p, Abc_BddNandConst0(), idk, 0 );
	      Abc_BddNandRemoveNode( p, id );
	      return 0;
	    }
	  j--;
	  continue;
	}
      Vec_IntPush( p->pvCFuncs[id], x );
    }
  return 0;
}
static inline int Abc_BddNandCspf( Abc_NandMan * p )
{
  int i, id;
  Vec_IntForEachEntryReverse( p->vObjs, id, i )
    {
      if ( Abc_BddNandObjIsDead( p, id ) )
	{
	  Abc_BddNandRemoveNode( p, id );
	  continue;
	}
      if ( Abc_BddNandGFunc( p, id ) )
	return -1;
      if ( Abc_BddNandCFuncCspf( p, id ) )
	return -1;
    }
  return Abc_BddNandBuildAll( p );
}
static inline int Abc_BddNandCspfFaninCone( Abc_NandMan * p, int startId )
{
  int i, id;
  Vec_Int_t * targets;
  targets = Vec_IntAlloc( 1 );
  if ( Abc_BddNandCFuncCspf( p, startId ) )
    return -1;
  Abc_BddNandDescendantSortedList( p, p->pvFanins, targets, startId );
  Vec_IntForEachEntryReverse( targets, id, i )
    {
      if ( Abc_BddNandObjIsDead( p, id ) )
	{
	  Abc_BddNandRemoveNode( p, id );
	  continue;
	}
      if ( Abc_BddNandGFunc( p, id ) )
	return -1;
      if ( Abc_BddNandCFuncCspf( p, id ) )
	return -1;
    }
  Vec_IntFree( targets );
  return 0;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Abc_BddNandIsFanoutShared_rec( Abc_NandMan * p, int id, int stage )
{
  int j, idj, m;
  if ( Abc_BddNandObjIsPo( p, id ) )
    return 0;
  m = p->pMark[id] >> 1;
  if ( m == stage )
    return 0;
  if ( m )
    return 1;
  p->pMark[id] += stage << 1;
  Vec_IntForEachEntry( p->pvFanouts[id], idj, j )
    if ( Abc_BddNandIsFanoutShared_rec( p, idj, stage ) )
      return 1;
  return 0;
}
static inline int Abc_BddNandIsFanoutShared( Abc_NandMan * p, int id )
{
  int i, j, idj, r;
  r = 0;
  Vec_IntForEachEntry( p->pvFanouts[id], idj, j )
    if ( Abc_BddNandIsFanoutShared_rec( p, idj, j + 1 ) )
      {
	r = 1;
	break;
      }
  for ( i = 0; i < p->nObjsAlloc; i++ )
    if ( p->pMark[i] % 2 )
      p->pMark[i] = 1;
    else
      p->pMark[i] = 0;
  return r;
}
static inline int Abc_BddNandGFuncMspf( Abc_NandMan * p, int id )
{
  int j, idj, idk;
  unsigned x, y;
  Vec_Int_t * posOld;
  if ( !Abc_BddNandIsFanoutShared( p, id ) )
    return Abc_BddNandGFunc( p, id );
  posOld = Vec_IntAlloc( Vec_IntSize( p->vPos ) );
  Vec_IntForEachEntry( p->vPos, idj, j )
    {
      idk = Vec_IntEntry( p->pvFanins[idj], 0 );
      Vec_IntPush( posOld, p->pBddFuncs[idk] );
    }
  if ( Abc_BddNandBuildFanoutConeInverted( p, id ) )
    {
      Vec_IntFree( posOld );
      return -1;
    }
  x = Abc_BddLitConst1();
  Vec_IntForEachEntry( p->vPos, idj, j )
    {
      idk = Vec_IntEntry( p->pvFanins[idj], 0 );
      if ( id == idk )
	y = Abc_BddXnor( p->pBdd, Abc_BddLitNot( p->pBddFuncs[idk] ), Vec_IntEntry( posOld, j ) );
      else
	y = Abc_BddXnor( p->pBdd, p->pBddFuncs[idk], Vec_IntEntry( posOld, j ) );
      y = Abc_BddOr( p->pBdd, y, p->pGFuncs[idj] );
      x = Abc_BddAnd( p->pBdd, x, y );
      if ( Abc_BddLitIsInvalid( x ) )
	{
	  Vec_IntFree( posOld );
	  return -1;
	}
    }
  p->pGFuncs[id] = x;
  Abc_BddNandBuildFanoutCone( p, id );
  Vec_IntFree( posOld );
  return 0;
}
static inline int Abc_BddNandCFuncMspf( Abc_NandMan * p, int id )
{
  int j, k, idj, idk;
  unsigned x, y;
  if ( !p->pvCFuncs[id] )
    p->pvCFuncs[id] = Vec_IntAlloc( Vec_IntSize( p->pvFanins[id] ) );
  Vec_IntClear( p->pvCFuncs[id] );
  Vec_IntForEachEntry( p->pvFanins[id], idj, j )
    {
      x = Abc_BddLitConst1();
      Vec_IntForEachEntry( p->pvFanins[id], idk, k )
	if ( k != j )
	  x = Abc_BddAnd( p->pBdd, x, p->pBddFuncs[idk] );
      x = Abc_BddOr( p->pBdd, Abc_BddLitNot( x ), p->pGFuncs[id] );
      y = Abc_BddOr( p->pBdd, x, p->pBddFuncs[idj] );
      if ( Abc_BddLitIsInvalid( y ) )
	return -1;
      if ( Abc_BddLitIsConst1( y ) )
	{
	  Abc_BddNandDisconnect( p, idj, id );
	  if ( !Vec_IntSize( p->pvFanins[id] ) )
	    {
	      Vec_IntForEachEntry( p->pvFanouts[id], idk, k )
		if ( Vec_IntFind( p->pvFanins[idk], Abc_BddNandConst0() ) == -1 )
		  Abc_BddNandConnect( p, Abc_BddNandConst0(), idk, 0 );
	      Abc_BddNandRemoveNode( p, id );
	    }
	  return 1;
	}
      Vec_IntPush( p->pvCFuncs[id], x );
    }
  return 0;
}
static inline int Abc_BddNandMspf( Abc_NandMan * p )
{
  int i, id, c;
  Vec_IntForEachEntryReverse( p->vObjs, id, i )
    {
      if ( Abc_BddNandObjIsDead( p, id ) )
	{
	  Abc_BddNandRemoveNode( p, id );
	  continue;
	}
      if ( Abc_BddNandGFuncMspf( p, id ) )
	return -1;
      c = Abc_BddNandCFuncMspf( p, id );
      if ( c == -1 )
	return -1;
      if ( c == 1 )
	{
	  Abc_BddNandBuildAll( p );
	  i = Vec_IntSize( p->vObjs );
	}
    }
  return 0;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Abc_BddNandTryConnect( Abc_NandMan * p, int fanin, int fanout )
{
  unsigned x;
  if ( Vec_IntFind( p->pvFanins[fanout], fanin ) != -1 )
    return 0; // already connected
  x = Abc_BddOr( p->pBdd, p->pBddFuncs[fanout], p->pGFuncs[fanout] );
  x = Abc_BddOr( p->pBdd, x, p->pBddFuncs[fanin] );
  if( Abc_BddLitIsInvalid( x ) )
    return -1;
  if ( Abc_BddLitIsConst1( x ) )
    {
      Abc_BddNandConnect( p, fanin, fanout, 1 );
      return 1;
    }
  return 0;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline void Abc_BddNandRefresh( Abc_NandMan * p )
{
  int out;
  abctime clk0 = 0;
  if ( p->nVerbose >= 2 )
    {
      printf( "Refresh\n" );
      clk0 = Abc_Clock();
    }
  while ( 1 )
    {
      Abc_BddManFree( p->pBdd );
      p->pBdd = Abc_BddManAlloc( Vec_IntSize( p->vPis ), 1 << p->nMem, 0, p->vOrdering, 0 );
      out = Abc_BddNandDc( p );
      if ( !out )
	out = Abc_BddNandBuildAll( p );
      if ( !out )
	{
	  if ( p->nMspf < 2 )
	    out = Abc_BddNandCspf( p );
	  else
	    out = Abc_BddNandMspf( p );
	}
      if ( !out )
	break;
      Abc_BddNandMemIncrease( p );
    }
  while ( p->pBdd->nObjs > 1 << (p->nMem - 1) )
    {
      Abc_BddNandMemIncrease( p );
      Abc_BddManRealloc( p->pBdd );
    }
  if ( p->nVerbose >= 2 )
    {
      printf( "Allocated by 2^%d\n", p->nMem );
      ABC_PRT( "Refresh took", Abc_Clock() - clk0 );
    }
}
static inline void Abc_BddNandRefreshIfNeeded( Abc_NandMan * p )
{
  if ( Abc_BddIsLimit( p->pBdd ) )
    Abc_BddNandRefresh( p );
}

static inline void Abc_BddNandBuild_Refresh( Abc_NandMan * p, int id ) { if ( Abc_BddNandBuild( p, id ) ) Abc_BddNandRefresh( p ); }
static inline void Abc_BddNandBuildAll_Refresh( Abc_NandMan * p ) { if ( Abc_BddNandBuildAll( p ) ) Abc_BddNandRefresh( p ); }
static inline void Abc_BddNandBuildFanoutCone_Refresh( Abc_NandMan * p, int startId ) { if ( Abc_BddNandBuildFanoutCone( p, startId ) ) Abc_BddNandRefresh( p ); }
static inline void Abc_BddNandCspf_Refresh( Abc_NandMan * p ) { if ( Abc_BddNandCspf( p ) ) Abc_BddNandRefresh( p ); }
static inline void Abc_BddNandCspfFaninCone_Refresh( Abc_NandMan * p, int startId ) { if ( Abc_BddNandCspfFaninCone( p, startId ) ) Abc_BddNandRefresh( p ); }
static inline void Abc_BddNandRemoveRedundantFanin_Refresh( Abc_NandMan * p, int id ) {
  if ( !Abc_BddNandRemoveRedundantFanin( p, id ) )
    return;
  Abc_BddNandRefresh( p );
  if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
    return;
  while ( Abc_BddNandRemoveRedundantFanin( p, id ) )
    {
      Abc_BddNandMemIncrease( p );
      Abc_BddNandRefresh( p );
      if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
	return;
    }
}
static inline int Abc_BddNandTryConnect_Refresh( Abc_NandMan * p, int fanin, int fanout )
{
  int c;
  c = Abc_BddNandTryConnect( p, fanin, fanout );
  if ( c == -1 )
    {
      Abc_BddNandRefresh( p );
      if ( Abc_BddNandObjIsEmptyOrDead( p, fanin ) )
	return 0;
      if ( Abc_BddNandObjIsEmptyOrDead( p, fanout ) )
	return 0;
      c = Abc_BddNandTryConnect( p, fanin, fanout );
    }
  while ( c == -1 )
    {
      Abc_BddNandMemIncrease( p );
      Abc_BddNandRefresh( p );
      if ( Abc_BddNandObjIsEmptyOrDead( p, fanin ) )
	return 0;
      if ( Abc_BddNandObjIsEmptyOrDead( p, fanout ) )
	return 0;
      c = Abc_BddNandTryConnect( p, fanin, fanout );
    }
  return c;
}
static inline void Abc_BddNandMspf_Refresh( Abc_NandMan * p )
{
  int out;
  abctime clk0 = 0;
  if ( p->nVerbose >= 2 )
    {
      printf( "Refresh mspf\n" );
      clk0 = Abc_Clock();
    }
  if ( !Abc_BddNandMspf( p ) )
    return;
  while ( 1 )
    {
      Abc_BddManFree( p->pBdd );
      p->pBdd = Abc_BddManAlloc( Vec_IntSize( p->vPis ), 1 << p->nMem, 0, p->vOrdering, 0 );
      out = Abc_BddNandDc( p );
      if ( !out )
	out = Abc_BddNandBuildAll( p );
      if ( !out )
	out = Abc_BddNandMspf( p );
      if ( !out )
	break;
      Abc_BddNandMemIncrease( p );
    }
  while ( p->pBdd->nObjs > 1 << (p->nMem - 1) )
    {
      Abc_BddNandMemIncrease( p );
      Abc_BddManRealloc( p->pBdd );
    }
  if ( p->nVerbose >= 2 )
    {
      printf( "Allocated by 2^%d\n", p->nMem );
      ABC_PRT( "Refresh took", Abc_Clock() - clk0 );
    }
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline void Abc_BddNandCspfEager( Abc_NandMan * p )
{
  int wires;
  wires = 0;
  while ( wires != Abc_BddNandCountWire( p ) )
    {
      wires = Abc_BddNandCountWire( p );
      Abc_BddNandRankAll( p );
      Abc_BddNandSortFaninAll( p );
      Abc_BddNandCspf_Refresh( p );
    }
}

/**Function*************************************************************
   
  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline void Abc_BddNandG1EagerReduce( Abc_NandMan * p, int id, int idj )
{
  int wire;
  wire = Abc_BddNandCountWire( p );
  Abc_BddNandCspfFaninCone_Refresh( p, id );
  if ( wire == Abc_BddNandCountWire( p ) )
    {
      Abc_BddNandDisconnect( p, idj, id );
      Abc_BddNandBuildFanoutCone_Refresh( p, id );
      if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
	return;
      Abc_BddNandCspfFaninCone_Refresh( p, id );
      return;
    }
  Abc_BddNandBuildAll_Refresh( p );
  if ( p->nMspf )
    Abc_BddNandMspf_Refresh( p );
  if ( p->nMspf < 2 )
    Abc_BddNandCspfEager( p );
}
static inline void Abc_BddNandG1WeakReduce( Abc_NandMan * p, int id, int idj )
{
  int wire;
  wire = Abc_BddNandCountWire( p );
  Abc_BddNandRemoveRedundantFanin_Refresh( p, id );
  if ( Abc_BddNandObjIsEmptyOrDead( p, id ) ||
       Abc_BddNandObjIsEmptyOrDead( p, idj ) )
    return; // If this, we don't need to do below.
  if ( wire == Abc_BddNandCountWire( p ) ) 
    Abc_BddNandDisconnect( p, idj, id );
  Abc_BddNandBuild_Refresh( p, id );
}
static inline void Abc_BddNandG1MspfReduce( Abc_NandMan * p, int id, int idj )
{
  Abc_BddNandBuildFanoutCone_Refresh( p, id );
  Abc_BddNandMspf_Refresh( p );
}
static inline void Abc_BddNandG1( Abc_NandMan * p, int fWeak, int fHalf )
{
  int i, j, id, idj;
  Vec_Int_t * targets, * targets2;
  targets = Vec_IntDup( p->vObjs );
  if ( fHalf )
    {
      targets2 = Vec_IntAlloc( 1 );
      Abc_BddNandMarkClear( p );  
      Vec_IntForEachEntryStart( p->vPos, id, i, Vec_IntSize( p->vPos ) / 2 )
	Abc_BddNandDescendantList_rec( p, p->pvFanins, targets2, id );
      Abc_BddNandSortList( p, targets2 );
    }
  else
    targets2 = Vec_IntDup( p->vObjs );
  Vec_IntForEachEntryReverse( targets, id, i )
    {
      if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
	continue;
      if ( p->nVerbose >= 3 )
	printf( "G1(2) for %d in %d gates\n", i, Vec_IntSize(targets) );
      Abc_BddNandMarkClear( p );
      p->pMark[id] = 1;
      Abc_BddNandMarkDescendant_rec( p, p->pvFanouts, id );
      // try connecting each pi if possible
      Vec_IntForEachEntry( p->vPis, idj, j )
	{
	  if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
	    break;
	  if ( Abc_BddNandTryConnect_Refresh( p, idj, id ) )
	    {
	      if ( fWeak )
		Abc_BddNandG1WeakReduce( p, id, idj );	
	      else if ( p->nMspf > 1 )
		Abc_BddNandG1MspfReduce( p, id, idj );	
	      else
		Abc_BddNandG1EagerReduce( p, id, idj );
	    }
	}
      // try connecting each candidate if possible
      Vec_IntForEachEntry( targets2, idj, j )
	{
	  if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
	    break;
	  if ( Abc_BddNandObjIsEmptyOrDead( p, idj ) )
	    continue;
	  if ( p->pMark[idj] )
	    continue;
	  if ( Abc_BddNandTryConnect_Refresh( p, idj, id ) )
	    {
	      if ( fWeak )
		Abc_BddNandG1WeakReduce( p, id, idj );
	      else if ( p->nMspf > 1 )
		Abc_BddNandG1MspfReduce( p, id, idj );
	      else
		Abc_BddNandG1EagerReduce( p, id, idj );
	    }
	}
      // recalculate fanouts for weak method
      if ( fWeak )
	{
	  if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
	    continue;
	  Abc_BddNandCspfFaninCone_Refresh( p, id );
	  if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
	    continue;
	  Abc_BddNandBuildAll_Refresh( p );
	}
    }
  Vec_IntFree( targets );
  Vec_IntFree( targets2 );
}

/**Function*************************************************************
   
  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline void Abc_BddNandG3( Abc_NandMan * p )
{
  int i, j, k, id, idj, idk, c, wire, new_id;
  unsigned fi, fj, f1, f0, gi, gj, x, y;
  Vec_Int_t * targets;
  c = 0; // for compile warning
  new_id = Vec_IntSize( p->vPis ) + 1;
  while ( !Abc_BddNandObjIsEmpty( p, new_id ) )
    {
      new_id++;
      if ( new_id >= p->nObjsAlloc )
	{
	  printf( "Error: Too many new merged nodes\n" );
	  abort();
	}
    }
  targets = Vec_IntDup( p->vObjs );
  Abc_BddNandCspf_Refresh( p );
  // optimize
  Vec_IntForEachEntryReverse( targets, id, i )
    {
      if ( !i )
	break;
      for ( j = i - 1; (j >= 0) && (((idj) = Vec_IntEntry(targets, j)), 1); j-- )
	{ //  Vec_IntForEachEntryReverseStart(targets, idj, j, i - 1)
	  Abc_BddNandRefreshIfNeeded( p );
	  if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
	    break;
	  if ( Abc_BddNandObjIsEmptyOrDead( p, idj ) )
	    continue;
	  if ( p->nVerbose >= 3 )
	    printf( "G3 between %d %d in %d gates\n", i, j, Vec_IntSize(targets) );
	  // calculate intersection. if it is impossible, continue.
	  fi = p->pBddFuncs[id];
	  fj = p->pBddFuncs[idj];
	  gi = p->pGFuncs[id];
	  gj = p->pGFuncs[idj];
	  f1 = Abc_BddAnd( p->pBdd, fi, fj );
	  f0 = Abc_BddAnd( p->pBdd, Abc_BddLitNot( fi ), Abc_BddLitNot( fj ) );
	  x = Abc_BddOr( p->pBdd, f1, f0 );
	  y = Abc_BddOr( p->pBdd, gi, gj );
	  x = Abc_BddOr( p->pBdd, x, y );
	  if ( !Abc_BddLitIsConst1( x ) )
	    continue;
	  // create BDD of intersection. both F and G.
	  x = Abc_BddAnd( p->pBdd, fi, Abc_BddLitNot( gi ) );
	  y = Abc_BddAnd( p->pBdd, fj, Abc_BddLitNot( gj ) );
	  x = Abc_BddOr( p->pBdd, x, y );
	  x = Abc_BddOr( p->pBdd, x, f1 );
	  y = Abc_BddAnd( p->pBdd, gi, gj );
	  if ( Abc_BddLitIsInvalid( x ) )
	    continue;
	  if ( Abc_BddLitIsInvalid( y ) )
	    continue;
	  p->pBddFuncs[new_id] = x;
	  p->pGFuncs[new_id] = y;
	  /*
	  unsigned x_ = x;
	  unsigned y_ = y;
	  */
	  p->pvFanins[new_id] = Vec_IntAlloc( 1 );
	  p->pvFanouts[new_id] = Vec_IntAlloc( 1 );
	  // for all living nodes, if it is not included in fanouts of i and j, and i and j themselves, try connect it to new node.
	  Abc_BddNandMarkClear( p );
	  p->pMark[id] = 1;
	  p->pMark[idj] = 1;
	  Abc_BddNandMarkDescendant_rec( p, p->pvFanouts, id );
	  Abc_BddNandMarkDescendant_rec( p, p->pvFanouts, idj );
	  x = Abc_BddOr( p->pBdd, Abc_BddLitNot( x ), y );
	  y = Abc_BddLitConst1();
	  Vec_IntForEachEntry( p->vPis, idk, k )
	    {
	      c = Abc_BddNandTryConnect( p, idk, new_id );
	      if ( c == 1 )
		{
		  if ( Abc_BddLitIsConst1( x ) ||
		       Abc_BddLitIsInvalid( x ) ||
		       Abc_BddLitIsInvalid( y ) )
		    break;
		  y = Abc_BddAnd( p->pBdd, y, p->pBddFuncs[idk] );
		  x = Abc_BddOr( p->pBdd, x, Abc_BddLitNot( y ) );
		}
	      else if ( c == -1 )
		break;
	    }
	  if ( c == -1 )
	    {
	      Abc_BddNandRemoveNode( p, new_id );
	      continue;
	    }
	  Vec_IntForEachEntry( targets, idk, k )
	    {
	      if ( Abc_BddNandObjIsEmptyOrDead( p, idk ) || p->pMark[idk] )
		continue;
	      c = Abc_BddNandTryConnect( p, idk, new_id );
	      if ( c == 1 )
		{
		  if ( Abc_BddLitIsConst1( x ) ||
		       Abc_BddLitIsInvalid( x ) ||
		       Abc_BddLitIsInvalid( y ) )
		    break;
		  y = Abc_BddAnd( p->pBdd, y, p->pBddFuncs[idk] );
		  x = Abc_BddOr( p->pBdd, x, Abc_BddLitNot( y ) );
		}
	      else if ( c == -1 )
		break;
	    }
	  // check the F of new node satisfies F and G.
	  if ( c == -1 || !Vec_IntSize( p->pvFanins[new_id] ) || !Abc_BddLitIsConst1( x ) )
	    {
	      Abc_BddNandRemoveNode( p, new_id );
	      continue;
	    }
	  /*
	  assert( Abc_BddOr( p->pBdd, Abc_BddOr( p->pBdd, x_, y_ ), y ) == 1 );
	  unsigned z = Abc_BddOr( p->pBdd, Abc_BddLitNot( x_ ), y_ );
	  z = Abc_BddOr( p->pBdd, z, Abc_BddLitNot( y ) );
	  assert( z == x );
	  */
	  // reduce the inputs
	  p->pBddFuncs[new_id] = Abc_BddLitNot( y );
	  Vec_IntForEachEntry( p->pvFanouts[id], idk, k )
	    Abc_BddNandConnect( p, new_id, idk, 0 );
	  Vec_IntForEachEntry( p->pvFanouts[idj], idk, k )
	    if ( Vec_IntFind( p->pvFanouts[new_id], idk ) == -1 )
	      Abc_BddNandConnect( p, new_id, idk, 0 );
	  Abc_BddNandObjEntry( p, new_id );
	  Abc_BddNandSortFanin( p, new_id );
	  c = Abc_BddNandRemoveRedundantFanin( p, new_id );
	  assert( !Abc_BddNandObjIsEmptyOrDead( p, new_id ) );
	  wire = Vec_IntSize( p->pvFanins[id] ) + Vec_IntSize( p->pvFanins[idj] );
	  if ( c || Vec_IntSize( p->pvFanins[new_id] ) > wire - 1 )
	    {
	      Abc_BddNandRemoveNode( p, new_id );
	      continue;
	    }
	  // if inputs < inputs_before - 1, do the followings
	  // remove merged (replaced) nodes
	  Abc_BddNandRemoveNode( p, id );
	  Abc_BddNandRemoveNode( p, idj );
	  // calculate function of new node
	  Abc_BddNandBuildFanoutCone_Refresh( p, new_id );
	  Abc_BddNandCspf_Refresh( p );
	  while ( !Abc_BddNandObjIsEmpty( p, new_id ) )
	    {
	      new_id++;
	      assert( new_id < p->nObjsAlloc );
	    }
	  break;
	}
    }
  Vec_IntFree( targets );
}

/**Function*************************************************************
   
  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline Gia_Man_t * Abc_BddNandNets2Gia( Vec_Ptr_t * vNets, Vec_Int_t * vPoCkts, Vec_Int_t * vPoIdxs, Vec_Int_t * vExternalCs, int fDc, Gia_Man_t * pOld )
{
  int i, j, k, id, idj, id0, id1, Value, cond, nPos;
  int * Values, * pPos;
  int ** vvPoValues;
  Gia_Obj_t * pObj;
  Gia_Man_t * pNew, * pTmp, * pDc;
  Abc_NandMan * p;
  vvPoValues = ABC_CALLOC( int *, Vec_PtrSize( vNets ) + 1 );
  pNew = Gia_ManStart( pOld->nObjs );
  pNew->pName = Abc_UtilStrsav( pOld->pName );
  pNew->pSpec = Abc_UtilStrsav( pOld->pSpec );
  Gia_ManHashAlloc( pNew );
  vvPoValues[0] = ABC_CALLOC( int, Gia_ManCiNum( pOld ) );
  Gia_ManForEachCi( pOld, pObj, i )
    vvPoValues[0][i] = Gia_ManAppendCi( pNew );
  Vec_PtrForEachEntryReverse( Abc_NandMan *, vNets, p, k )
    {
      Values = ABC_CALLOC( int, p->nObjsAlloc );
      vvPoValues[k+1] = ABC_CALLOC( int, Vec_IntSize( p->vPos ) );
      id = Abc_BddNandConst0();
      Values[id] = Gia_ManConst0Lit( pNew );
      Vec_IntForEachEntry( p->vPis, id, i )
	Values[id] = vvPoValues[Vec_IntEntry( p->vPiCkts, i ) + 1][Vec_IntEntry( p->vPiIdxs, i )];
      Vec_IntForEachEntry( p->vObjs, id, i )
	{
	  if ( Abc_BddNandObjIsEmptyOrDead( p, id ) )
	    continue;
	  if ( Vec_IntSize( p->pvFanins[id] ) == 1 )
	    {
	      id0 = Vec_IntEntry( p->pvFanins[id], 0 );
	      Values[id] = Abc_LitNot( Values[id0] );
	    }
	  else // if ( Vec_IntSize( p->pvFanins[id] ) >= 2 )
	    {
	      id0 = Vec_IntEntry( p->pvFanins[id], 0 );
	      id1 = Vec_IntEntry( p->pvFanins[id], 1 );
	      Values[id] = Gia_ManHashAnd( pNew, Values[id0], Values[id1] );
	      Vec_IntForEachEntryStart( p->pvFanins[id], idj, j, 2 )
		Values[id] = Gia_ManHashAnd( pNew, Values[id], Values[idj] );
	      Values[id] = Abc_LitNot( Values[id] );
	    }
	}
      Vec_IntForEachEntry( p->vPos, id, i )
	{
	  id0 = Vec_IntEntry( p->pvFanins[id], 0 );
	  Value = Values[id0];
	  vvPoValues[k+1][i] = Value;
	}
      ABC_FREE( Values );
    }
  Vec_IntForEachEntry( vExternalCs, cond, i )
    {
      if ( Vec_IntEntry( vPoCkts, i ) < -1 )
	Value = Gia_ManConst0Lit( pNew );
      else
	Value = vvPoValues[Vec_IntEntry( vPoCkts, i ) + 1][Vec_IntEntry( vPoIdxs, i )];
      Gia_ManAppendCo( pNew, Abc_LitNotCond( Value, cond ) );
    }
  pNew = Gia_ManCleanup( pTmp = pNew );
  Gia_ManStop( pTmp );
  if ( fDc )
    {
      nPos = Gia_ManCoNum( pOld ) / 2;
      pPos = ABC_CALLOC( int, nPos );
      for ( i = 0; i < nPos; i++ )
	pPos[i] = nPos + i;
      pDc = Gia_ManDupCones( pOld, pPos, nPos, 0 );
      ABC_FREE( pPos );
      Gia_ManDupAppendShare( pNew, pDc );
      Gia_ManStop( pDc );
    }
  for ( i = 0; i < Vec_PtrSize( vNets ) + 1; i++ )
    ABC_FREE( vvPoValues[i] );
  ABC_FREE( vvPoValues );
  return pNew;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline void Abc_BddNandPropagateDc( Vec_Ptr_t * vNets, int from, Gia_Man_t * pGlobal, int nDcPropagate )
{
  int i, j, k, l, id, idj, pi, index, nPos, flag, count;
  int * pPos;
  unsigned x, y;
  Vec_Int_t * vVars, * vNodes;
  Vec_Int_t ** pvPis;
  Abc_NandMan * pFrom, * pTo;
  Gia_Man_t * pDc, * pTmp, * pBase;
  vVars = Vec_IntAlloc( 1 );
  vNodes = Vec_IntAlloc( 1 );
  pFrom = Vec_PtrEntry( vNets, from );
  pvPis = ABC_CALLOC( Vec_Int_t *, Vec_PtrSize( vNets ) );
  for ( i = 0; i < Vec_PtrSize( vNets ); i++ )
    pvPis[i] = Vec_IntAlloc( 1 );
  Vec_IntForEachEntry( pFrom->vPis, id, pi )
    if ( Vec_IntEntry( pFrom->vPiCkts, pi ) >= 0 )
      Vec_IntPush( pvPis[Vec_IntEntry( pFrom->vPiCkts, pi )], pi );
  Vec_PtrForEachEntry( Abc_NandMan *, vNets, pTo, k )
    {
      if ( !Vec_IntSize( pvPis[k] ) )
	continue;
      Vec_IntForEachEntry( pvPis[k], pi, i )
	{
	  id = Vec_IntEntry( pFrom->vPis, pi );
	  x = Abc_BddLitConst1();
	  // calculate AND of G of fanouts
	  Vec_IntForEachEntry( pFrom->pvFanouts[id], idj, j )
	    {
	      if ( Abc_BddNandObjIsPo( pFrom, idj ) )
		x = Abc_BddAnd( pFrom->pBdd, x, pFrom->pGFuncs[idj] );
	      else
		{
		  index = Vec_IntFind( pFrom->pvFanins[idj], id );
		  y = Vec_IntEntry( pFrom->pvCFuncs[idj], index );
		  x = Abc_BddAnd( pFrom->pBdd, x, y );
		}
	    }
	  while ( Abc_BddLitIsInvalid( x ) )
	    {
	      Abc_BddNandRefresh( pFrom );
	      x = Abc_BddLitConst1();
	      Vec_IntForEachEntry( pFrom->pvFanouts[id], idj, j )
		{
		  if ( Abc_BddNandObjIsPo( pFrom, idj ) )
		    x = Abc_BddAnd( pFrom->pBdd, x, pFrom->pGFuncs[idj] );
		  else
		    {
		      index = Vec_IntFind( pFrom->pvFanins[idj], id );
		      y = Vec_IntEntry( pFrom->pvCFuncs[idj], index );
		      x = Abc_BddAnd( pFrom->pBdd, x, y );
		    }
		}
	      if ( Abc_BddLitIsInvalid( x ) )
		Abc_BddNandMemIncrease( pFrom );
	    }
	  Vec_IntClear( vNodes );
	  Vec_IntPush( vNodes, x );
	  pDc = Abc_BddGenGia( pFrom->pBdd, vNodes );
	  if ( nDcPropagate == 1 )
	    {
	      // Universally quantify unused inputs
	      Vec_IntClear( vVars );
	      for ( pi = 0; pi < Vec_IntSize( pFrom->vPis ); pi++ )	
		if ( Vec_IntFind( pvPis[k], pi ) == -1 )
		  {
		    pTmp = pDc;
		    pDc = Gia_ManDupUniv( pDc, pi );
		    Gia_ManStop( pTmp );
		    Vec_IntPush( vVars, pi );
		  }
	      // Add inputs to match the number of inputs
	      while( Gia_ManCiNum( pDc ) < Vec_IntSize( pTo->vPos ) )
		{
		  Vec_IntPush( vVars, Gia_ManCiNum( pDc ) );
		  Gia_ManAppendCi( pDc );
		}
	      // Permitate inputs to match them with the outputs of the next partition
	      Vec_IntClear( vNodes );
	      count = 0;
	      for ( j = 0; j < Gia_ManCiNum( pDc ); j++ )
		{
		  flag = 0;
		  Vec_IntForEachEntry( pvPis[k], pi, l )
		    if ( j == Vec_IntEntry( pFrom->vPiIdxs, pi ) )
		      {
			Vec_IntPush( vNodes, pi );
			flag = 1;
			break;
		      }
		  if ( flag )
		    continue;
		  Vec_IntPush( vNodes, Vec_IntEntry( vVars, count ) );
		  count++;
		}
	      pTmp = pDc;
	      pDc = Gia_ManDupPerm( pDc, vNodes );
	      Gia_ManStop( pTmp );
	      // remove unused inputs
	      pTmp = pDc;
	      pDc = Gia_ManDupRemovePis( pDc, Gia_ManCiNum( pDc ) - Vec_IntSize( pTo->vPos ) );
	      Gia_ManStop( pTmp );
	      // Place it on top of the next partition
	      pTmp = pDc;
	      pDc = Gia_ManDupOntop( pTo->pGia, pDc );
	      Gia_ManStop( pTmp );
	      Vec_PtrPush( Vec_PtrEntry( pTo->vvDcGias, Vec_IntEntry( pFrom->vPiIdxs, pi ) ), pDc );
	      continue;
	    }
	  // if nDcPropagate >= 2
	  // create a circuit with inputs of the next circuit and outputs of the previous circuit
	  pBase = Abc_BddNandGiaExpand( pGlobal, pTo->vOrgPis, pFrom->vOrgPis );
	  nPos = Vec_IntSize( pFrom->vOrgPis );
	  pPos = ABC_CALLOC( int, nPos );
	  for ( j = 0; j < nPos; j++ )
	    pPos[j] = Gia_ManCoNum( pGlobal ) + j;
	  pTmp = pBase;
	  pBase = Gia_ManDupCones( pBase, pPos, nPos, 0 );
	  Gia_ManStop( pTmp );
	  ABC_FREE( pPos );
	  // create a DC circuit in terms of inputs of the next circuit
	  pTmp = pDc;
	  pDc = Gia_ManDupOntop( pBase, pDc );
	  Gia_ManStop( pBase );
	  Gia_ManStop( pTmp );
	  // remove unnecessary inputs by universing it
	  for ( j = 0; j < Gia_ManCiNum( pGlobal ); j++ )
	    {
	      pTmp = pDc;
	      pDc = Gia_ManDupUniv( pDc, j );
	      Gia_ManStop( pTmp );
           }
	  pTmp = pDc;
	  pDc = Gia_ManDupLastPis( pDc, Vec_IntSize( pTo->vOrgPis ) );
	  Gia_ManStop( pTmp );
	  
	  // push it to dc list
	  Vec_PtrPush( Vec_PtrEntry( pTo->vvDcGias, Vec_IntEntry( pFrom->vPiIdxs, pi ) ), pDc );
	}
    }
  Vec_IntFree( vVars );
  Vec_IntFree( vNodes );
  for ( i = 0; i < Vec_PtrSize( vNets ); i++ )
    Vec_IntFree( pvPis[i] );
  ABC_FREE( pvPis );
}

/**Function*************************************************************
   
  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline void Abc_BddNandPrintStats( Abc_NandMan * p, char * prefix, abctime clk0 )
{
  printf( "\r%-10s: gates = %5d, wires = %5d, AIG node = %5d", prefix, Vec_IntSize( p->vObjs ), Abc_BddNandCountWire( p ), Abc_BddNandCountWire( p ) - Vec_IntSize( p->vObjs ) );
  ABC_PRT( ", time ", Abc_Clock() - clk0 );
}
Gia_Man_t * Abc_BddNandGiaTest( Gia_Man_t * pGia, int nMem, int fReo, int nType, int fRm, int fRep, int fDc, int fSpec, int nWindowSize,int nDcPropagate, int nMspf, int nVerbose )
{
  int i, k, id, nPos;
  int * pPos;
  Abc_NandMan * p;
  Gia_Obj_t * pObj;
  Gia_Man_t * pNew;
  Vec_Ptr_t * vNets, * vDcGias;
  Vec_Int_t * vPoCkts, * vPoIdxs, * vExternalCs, * vFuncs;
  vNets = Vec_PtrAlloc( 1 );
  vPoCkts = Vec_IntAlloc( 1 );
  vPoIdxs = Vec_IntAlloc( 1 );
  vExternalCs = Vec_IntAlloc( 1 );
  nPos = Gia_ManCoNum( pGia ); // for compile warning
  if ( !nWindowSize )
    {
      if ( fDc )
	{
	  nPos = Gia_ManCoNum( pGia ) / 2;
	  pPos = ABC_CALLOC( int, nPos );
	  for ( i = 0; i < nPos; i++ )
	    pPos[i] = i;
	  pNew = Gia_ManDupCones( pGia, pPos, nPos, 0 );
	  ABC_FREE( pPos );
	}
      else
	pNew = Gia_ManDup( pGia );
      p = Abc_BddNandManAlloc( pNew, nMem, fRm, nMspf, nVerbose );
      Vec_IntForEachEntry( p->vPis, id, i )
	{
	  Vec_IntPush( p->vPiCkts, -1 );
	  Vec_IntPush( p->vPiIdxs, i );
	}
      Vec_PtrPush( vNets, p );
      Gia_ManForEachCo( pNew, pObj, i )
	{
	  Vec_IntPush( vExternalCs, 0 );
	  Vec_IntPush( vPoCkts, 0 );
	  Vec_IntPush( vPoIdxs, i );
	}
      if ( fDc )
	for ( i = nPos; i < nPos * 2; i++ )
	  {
	    vDcGias = Vec_PtrEntry( p->vvDcGias, i - nPos );
	    pNew = Gia_ManDupCones( pGia, &i, 1, 0 );
	    Vec_PtrPush( vDcGias, pNew );
	  }
    }
  else
    Abc_BddNandGia2Nets( pGia, vNets, vPoCkts, vPoIdxs, vExternalCs, nMem, fRm, fDc, nWindowSize, nMspf, nVerbose );
  // optimize
  abctime clk0 = Abc_Clock();
  Vec_PtrForEachEntry( Abc_NandMan *, vNets, p, i )
    {
      p->pBdd = Abc_BddManAlloc( Vec_IntSize( p->vPis ), 1 << p->nMem, 1, NULL, 0 );
      p->nMem = Abc_Base2Log( p->pBdd->nObjsAlloc );
      while ( Abc_BddNandDc( p ) || Abc_BddNandBuildAll( p ) )
	{
	  Abc_BddNandMemIncrease( p );
	  Abc_BddManFree( p->pBdd );
	  p->pBdd = Abc_BddManAlloc( Vec_IntSize( p->vPis ), 1 << p->nMem, 0, NULL, 0 );
	}
      while ( p->pBdd->nObjs > 1 << (p->nMem - 1) )
	{
	  Abc_BddNandMemIncrease( p );
	  Abc_BddManRealloc( p->pBdd );
	}
      if ( fReo )
	{
	  vFuncs = Vec_IntAlloc( 1 );
	  Vec_IntForEachEntry( p->vObjs, id, k )
	    Vec_IntPush( vFuncs, p->pBddFuncs[id] );
	  Vec_IntForEachEntry( p->vPos, id, k )
	    Vec_IntPush( vFuncs, p->pGFuncs[id] );
	  Abc_BddReorderConfig( p->pBdd, 10 );
	  p->pBdd->fGC = 1;
	  p->pBdd->fRealloc = 1;
	  Abc_BddReorder( p->pBdd, vFuncs );
	  Abc_BddReorderConfig( p->pBdd, 0 );
	  p->pBdd->fGC = 0;
	  p->pBdd->fRealloc = 0;
	  p->vOrdering = Vec_IntDup( p->pBdd->vOrdering );
	  Vec_IntFree( vFuncs );
	  p->nMem = Abc_Base2Log( p->pBdd->nObjsAlloc );
	}
      if ( nVerbose >= 2 )
	printf( "Allocated by 2^%d\n", p->nMem );
      if ( nVerbose )
	Abc_BddNandPrintStats( p, "initial", clk0 );
      if ( p->nMspf )
	Abc_BddNandMspf_Refresh( p );
      if ( p->nMspf < 2 )
	Abc_BddNandCspfEager( p );
      if ( nVerbose )
	Abc_BddNandPrintStats( p, "pf", clk0 );
      int wire = 0;
      while ( wire != Abc_BddNandCountWire( p ) )
	{
	  wire = Abc_BddNandCountWire( p );
	  switch ( nType ) {
	  case 0:
	    break;
	  case 1:
	    Abc_BddNandG1( p, 0, fSpec );
	    if ( nVerbose ) Abc_BddNandPrintStats( p, "G1", clk0 );
	    break;
	  case 2:
	    Abc_BddNandG1( p, 1, fSpec );
	    if ( nVerbose ) Abc_BddNandPrintStats( p, "G2", clk0 );
	    break;
	  case 3:
	    Abc_BddNandG3( p );
	    if ( nVerbose ) Abc_BddNandPrintStats( p, "G3", clk0 );
	    break;
	  default:
	    printf( "Error: Invalid optimization type %d\n", nType );
	    abort();
	  }
	  if ( p->nMspf )
	    Abc_BddNandMspf_Refresh( p );
	  if ( p->nMspf < 2 )
	    Abc_BddNandCspfEager( p );
	  if ( !fRep )
	    break;
	}
      if ( nWindowSize && nDcPropagate )
	{
	  Abc_BddNandCspfEager( p );
	  Abc_BddNandPropagateDc( vNets, i, pGia, nDcPropagate );
	}
    }
  if ( nVerbose )
    ABC_PRT( "total ", Abc_Clock() - clk0 );
  pNew = Abc_BddNandNets2Gia( vNets, vPoCkts, vPoIdxs, vExternalCs, fDc, pGia );
  Vec_IntFree( vPoCkts );
  Vec_IntFree( vPoIdxs );
  Vec_IntFree( vExternalCs );
  Vec_PtrForEachEntry( Abc_NandMan *, vNets, p, i )
    Abc_BddNandManFree( p );
  Vec_PtrFree( vNets );
  return pNew;
}

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END

