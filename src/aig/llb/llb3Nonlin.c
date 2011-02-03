/**CFile****************************************************************

  FileName    [llb2Nonlin.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [BDD based reachability.]

  Synopsis    [Non-linear quantification scheduling.]

  Author      [Alan Mishchenko]
  
  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - June 20, 2005.]

  Revision    [$Id: llb2Nonlin.c,v 1.00 2005/06/20 00:00:00 alanmi Exp $]

***********************************************************************/

#include "llbInt.h"

ABC_NAMESPACE_IMPL_START
 

////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

typedef struct Llb_Mnn_t_ Llb_Mnn_t;
struct Llb_Mnn_t_
{
    Aig_Man_t *     pInit;          // AIG manager
    Aig_Man_t *     pAig;           // AIG manager
    Gia_ParLlb_t *  pPars;          // parameters

    DdManager *     dd;             // BDD manager
    DdManager *     ddG;            // BDD manager
    DdManager *     ddR;            // BDD manager
    Vec_Ptr_t *     vRings;         // onion rings in ddR

    Vec_Ptr_t *     vLeaves;        
    Vec_Ptr_t *     vRoots;
    int *           pVars2Q;
    int *           pOrder;

    Vec_Int_t *     vCs2Glo;        // cur state variables into global variables
    Vec_Int_t *     vNs2Glo;        // next state variables into global variables
    Vec_Int_t *     vGlo2Cs;        // global variables into cur state variables
    Vec_Int_t *     vGlo2Ns;        // global variables into next state variables

    int             timeImage;
    int             timeTran1;
    int             timeTran2;
    int             timeGloba;
    int             timeOther;
    int             timeTotal;
    int             timeReo;
    int             timeReoG;

};

extern int timeBuild, timeAndEx, timeOther;
extern int nSuppMax;

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

/**Function*************************************************************

  Synopsis    [Finds variable whose 0-cofactor is the smallest.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Llb_NonlinFindBestVar( DdManager * dd, DdNode * bFunc, Aig_Man_t * pAig )
{
    int fVerbose = 0;
    Aig_Obj_t * pObj;
    DdNode * bCof, * bVar;
    int i, iVar, iVarBest = -1, iValue, iValueBest = ABC_INFINITY, Size0Best = -1;
    int Size, Size0, Size1;
    int clk = clock();
    Size = Cudd_DagSize(bFunc);
//    printf( "Original = %6d.  SuppSize = %3d. Vars = %3d.\n", 
//        Size = Cudd_DagSize(bFunc), Cudd_SupportSize(dd, bFunc), Aig_ManRegNum(pAig) );
    Saig_ManForEachLo( pAig, pObj, i )
    {
        iVar = Aig_ObjId(pObj);

if ( fVerbose )
printf( "Var =%3d : ", iVar );
        bVar = Cudd_bddIthVar(dd, iVar);

        bCof = Cudd_bddAnd( dd, bFunc, Cudd_Not(bVar) );          Cudd_Ref( bCof );
        Size0 = Cudd_DagSize(bCof);
if ( fVerbose )
printf( "Supp0 =%3d  ",  Cudd_SupportSize(dd, bCof) );
if ( fVerbose )
printf( "Size0 =%6d   ", Size0 );
        Cudd_RecursiveDeref( dd, bCof );

        bCof = Cudd_bddAnd( dd, bFunc, bVar );                    Cudd_Ref( bCof );
        Size1 = Cudd_DagSize(bCof);
if ( fVerbose )
printf( "Supp1 =%3d  ",  Cudd_SupportSize(dd, bCof) );
if ( fVerbose )
printf( "Size1 =%6d   ", Size1 );
        Cudd_RecursiveDeref( dd, bCof );

        iValue = ABC_MAX(Size0, Size1) - ABC_MIN(Size0, Size1) + Size0 + Size1 - Size;
if ( fVerbose )
printf( "D =%6d  ", Size0 + Size1 - Size );
if ( fVerbose )
printf( "B =%6d  ", ABC_MAX(Size0, Size1) - ABC_MIN(Size0, Size1) );
if ( fVerbose )
printf( "S =%6d\n", iValue );
        if ( Size0 > 1 && Size1 > 1 && iValueBest > iValue )
        {
            iValueBest = iValue;
            iVarBest   = i;
            Size0Best  = Size0;
        }
    }
    printf( "BestVar = %4d/%4d.  Value =%6d.  Orig =%6d. Size0 =%6d. ", 
        iVarBest, Aig_ObjId(Saig_ManLo(pAig,iVarBest)), iValueBest, Size, Size0Best );
    Abc_PrintTime( 1, "Time", clock() - clk );
    return iVarBest;
}


/**Function*************************************************************

  Synopsis    [Finds variable whose 0-cofactor is the smallest.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Llb_NonlinTrySubsetting( DdManager * dd, DdNode * bFunc )
{
    DdNode * bNew;
    printf( "Original = %6d.  SuppSize = %3d.    ", 
        Cudd_DagSize(bFunc), Cudd_SupportSize(dd, bFunc) );
    bNew = Cudd_SubsetHeavyBranch( dd, bFunc, Cudd_SupportSize(dd, bFunc), 1000 );  Cudd_Ref( bNew );
    printf( "Result   = %6d.  SuppSize = %3d.\n", 
        Cudd_DagSize(bNew), Cudd_SupportSize(dd, bNew) );
    Cudd_RecursiveDeref( dd, bNew );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Llb_NonlinPrepareVarMap( Llb_Mnn_t * p )
{
    Aig_Obj_t * pObjLi, * pObjLo, * pObj;
    int i, iVarLi, iVarLo;
    p->vCs2Glo = Vec_IntStartFull( Aig_ManObjNumMax(p->pAig) );
    p->vNs2Glo = Vec_IntStartFull( Aig_ManObjNumMax(p->pAig) );
    p->vGlo2Cs = Vec_IntStartFull( Aig_ManRegNum(p->pAig) );
    p->vGlo2Ns = Vec_IntStartFull( Aig_ManRegNum(p->pAig) );
    Saig_ManForEachLiLo( p->pAig, pObjLi, pObjLo, i )
    {
        iVarLi = Aig_ObjId(pObjLi);
        iVarLo = Aig_ObjId(pObjLo);
        assert( iVarLi >= 0 && iVarLi < Aig_ManObjNumMax(p->pAig) );
        assert( iVarLo >= 0 && iVarLo < Aig_ManObjNumMax(p->pAig) );
        Vec_IntWriteEntry( p->vCs2Glo, iVarLo, i );
        Vec_IntWriteEntry( p->vNs2Glo, iVarLi, i );
        Vec_IntWriteEntry( p->vGlo2Cs, i, iVarLo );
        Vec_IntWriteEntry( p->vGlo2Ns, i, iVarLi );
    }
    // add mapping of the PIs
    Saig_ManForEachPi( p->pAig, pObj, i )
    {
        Vec_IntWriteEntry( p->vCs2Glo, Aig_ObjId(pObj), Aig_ManRegNum(p->pAig)+i );
        Vec_IntWriteEntry( p->vNs2Glo, Aig_ObjId(pObj), Aig_ManRegNum(p->pAig)+i );
    }
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
DdNode * Llb_NonlinComputeInitState( Aig_Man_t * pAig, DdManager * dd )
{
    Aig_Obj_t * pObj;
    DdNode * bRes, * bVar, * bTemp;
    int i, iVar;
    bRes = Cudd_ReadOne( dd );   Cudd_Ref( bRes );
    Saig_ManForEachLo( pAig, pObj, i )
    {
        iVar = (Cudd_ReadSize(dd) == Aig_ManRegNum(pAig)) ? i : Aig_ObjId(pObj);
        bVar = Cudd_bddIthVar( dd, iVar );
        bRes = Cudd_bddAnd( dd, bTemp = bRes, Cudd_Not(bVar) );  Cudd_Ref( bRes );
        Cudd_RecursiveDeref( dd, bTemp );
    }
    Cudd_Deref( bRes );
    return bRes;
}


/**Function*************************************************************

  Synopsis    [Derives counter-example by backward reachability.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Abc_Cex_t * Llb_NonlinDeriveCex( Llb_Mnn_t * p )
{
    extern Abc_Cex_t * Ssw_SmlAllocCounterExample( int nRegs, int nRealPis, int nFrames );
    extern int Ssw_SmlFindOutputCounterExample( Aig_Man_t * pAig, Abc_Cex_t * p );
    Abc_Cex_t * pCex;
    Aig_Obj_t * pObj;
    Vec_Int_t * vVarsNs;
    DdNode * bState, * bImage, * bOneCube, * bTemp, * bRing;
    int i, v, RetValue, nPiOffset;
    char * pValues = ABC_ALLOC( char, Cudd_ReadSize(p->ddR) );
    assert( Vec_PtrSize(p->vRings) > 0 );

    // update quantifiable vars
    memset( p->pVars2Q, 0, sizeof(int) * Cudd_ReadSize(p->dd) );
    vVarsNs = Vec_IntAlloc( Aig_ManRegNum(p->pAig) );
    Saig_ManForEachLi( p->pAig, pObj, i )
    {
        p->pVars2Q[Aig_ObjId(pObj)] = 1;
        Vec_IntPush( vVarsNs, Aig_ObjId(pObj) );
    }
/*
    Saig_ManForEachLo( p->pAig, pObj, i )
        printf( "%d ", pObj->Id );
    printf( "\n" );
    Saig_ManForEachLi( p->pAig, pObj, i )
        printf( "%d(%d) ", pObj->Id, Aig_ObjFaninId0(pObj) );
    printf( "\n" );
*/
    // allocate room for the counter-example
    pCex = Ssw_SmlAllocCounterExample( Saig_ManRegNum(p->pAig), Saig_ManPiNum(p->pAig), Vec_PtrSize(p->vRings) );
    pCex->iFrame = Vec_PtrSize(p->vRings) - 1;
    pCex->iPo = -1;

    // get the last cube
    bOneCube = Cudd_bddIntersect( p->ddR, Vec_PtrEntryLast(p->vRings), p->ddR->bFunc );  Cudd_Ref( bOneCube );
    RetValue = Cudd_bddPickOneCube( p->ddR, bOneCube, pValues );
    Cudd_RecursiveDeref( p->ddR, bOneCube );
    assert( RetValue );

    // write PIs of counter-example
    nPiOffset = Saig_ManRegNum(p->pAig) + Saig_ManPiNum(p->pAig) * (Vec_PtrSize(p->vRings) - 1);
    Saig_ManForEachPi( p->pAig, pObj, i )
        if ( pValues[Saig_ManRegNum(p->pAig)+i] == 1 )
            Aig_InfoSetBit( pCex->pData, nPiOffset + i );

    // write state in terms of NS variables
    if ( Vec_PtrSize(p->vRings) > 1 )
    {
        bState = Llb_CoreComputeCube( p->dd, vVarsNs, 1, pValues );   Cudd_Ref( bState );
    }
    // perform backward analysis
    Vec_PtrForEachEntryReverse( DdNode *, p->vRings, bRing, v )
    { 
        if ( v == Vec_PtrSize(p->vRings) - 1 )
            continue;
//Extra_bddPrintSupport( p->dd, bState );  printf( "\n" );
//Extra_bddPrintSupport( p->dd, bRing );   printf( "\n" );
        // compute the next states
        bImage = Llb_NonlinImage( p->pAig, p->vLeaves, p->vRoots, p->pVars2Q, p->dd, bState, 
            p->pPars->fReorder, p->pPars->fVeryVerbose, p->pOrder, ABC_INFINITY ); // consumed reference
        assert( bImage != NULL );
        Cudd_Ref( bImage );
//Extra_bddPrintSupport( p->dd, bImage );  printf( "\n" );

        // move reached states into ring manager
        bImage = Extra_TransferPermute( p->dd, p->ddR, bTemp = bImage, Vec_IntArray(p->vCs2Glo) );    Cudd_Ref( bImage );
        Cudd_RecursiveDeref( p->dd, bTemp );

        // intersect with the previous set
        bOneCube = Cudd_bddIntersect( p->ddR, bImage, bRing );                Cudd_Ref( bOneCube );
        Cudd_RecursiveDeref( p->ddR, bImage );

        // find any assignment of the BDD
        RetValue = Cudd_bddPickOneCube( p->ddR, bOneCube, pValues );
        Cudd_RecursiveDeref( p->ddR, bOneCube );
        assert( RetValue );

        // write PIs of counter-example
        nPiOffset -= Saig_ManPiNum(p->pAig);
        Saig_ManForEachPi( p->pAig, pObj, i )
            if ( pValues[Saig_ManRegNum(p->pAig)+i] == 1 )
                Aig_InfoSetBit( pCex->pData, nPiOffset + i );

        // check that we get the init state
        if ( v == 0 )
        {
            Saig_ManForEachLo( p->pAig, pObj, i )
                assert( pValues[i] == 0 );
            break;
        }

        // write state in terms of NS variables
        bState = Llb_CoreComputeCube( p->dd, vVarsNs, 1, pValues );   Cudd_Ref( bState );
    }
    assert( nPiOffset == Saig_ManRegNum(p->pAig) );
    // update the output number
    RetValue = Ssw_SmlFindOutputCounterExample( p->pInit, pCex );
    assert( RetValue >= 0 && RetValue < Saig_ManPoNum(p->pInit) ); // invalid CEX!!!
    pCex->iPo = RetValue;
    // cleanup
    ABC_FREE( pValues );
    Vec_IntFree( vVarsNs );
    return pCex;
}

/**Function*************************************************************

  Synopsis    [Perform reachability with hints.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Llb_NonlinReachability( Llb_Mnn_t * p )
{ 
    DdNode * bCurrent, * bNext, * bTemp;
    int iVar, nIters, nBddSize0, nBddSize, Limit = p->pPars->nBddMax;
    int clk2, clk3, clk = clock();
    assert( Aig_ManRegNum(p->pAig) > 0 );

    // compute time to stop
    if ( p->pPars->TimeLimit )
        p->pPars->TimeTarget = clock() + p->pPars->TimeLimit * CLOCKS_PER_SEC;
    else
        p->pPars->TimeTarget = 0;

    // create bad state in the ring manager
    p->ddR->bFunc  = Llb_BddComputeBad( p->pInit, p->ddR );          Cudd_Ref( p->ddR->bFunc );
    // compute the starting set of states
    bCurrent       = Llb_NonlinComputeInitState( p->pAig, p->dd );   Cudd_Ref( bCurrent );
    p->ddG->bFunc  = Llb_NonlinComputeInitState( p->pAig, p->ddG );  Cudd_Ref( p->ddG->bFunc );  // reached
    p->ddG->bFunc2 = Llb_NonlinComputeInitState( p->pAig, p->ddG );  Cudd_Ref( p->ddG->bFunc2 ); // frontier 
    for ( nIters = 0; nIters < p->pPars->nIterMax; nIters++ )
    { 
        // check the runtime limit
        clk2 = clock();
        if ( p->pPars->TimeLimit && clock() >= p->pPars->TimeTarget )
        {
            if ( !p->pPars->fSilent )
                printf( "Reached timeout during image computation (%d seconds).\n",  p->pPars->TimeLimit );
            p->pPars->iFrame = nIters - 1;
            Cudd_RecursiveDeref( p->dd,  bCurrent );  bCurrent = NULL;
            return -1;
        }

        // save the onion ring
        bTemp = Extra_TransferPermute( p->dd, p->ddR, bCurrent, Vec_IntArray(p->vCs2Glo) );    Cudd_Ref( bTemp );
        Vec_PtrPush( p->vRings, bTemp );

        // check it for bad states
        if ( !p->pPars->fSkipOutCheck && !Cudd_bddLeq( p->ddR, bTemp, Cudd_Not(p->ddR->bFunc) ) ) 
        {
            assert( p->pInit->pSeqModel == NULL );
            if ( !p->pPars->fBackward )
                p->pInit->pSeqModel = Llb_NonlinDeriveCex( p ); 
            Cudd_RecursiveDeref( p->dd,  bCurrent );  bCurrent = NULL;
            if ( !p->pPars->fSilent )
            {
                if ( !p->pPars->fBackward )
                    printf( "Output %d was asserted in frame %d (use \"write_counter\" to dump a witness).  ", p->pInit->pSeqModel->iPo, nIters );
                else
                    printf( "Output ??? was asserted in frame %d (counter-example is not produced).  ", nIters );
                Abc_PrintTime( 1, "Time", clock() - clk );
            }
            return 0;
        }

        // check the size
        if ( Cudd_DagSize(bCurrent) > p->pPars->nBddMax )
        {
            DdNode * bVar, * bVarG;

            // find the best variable
            iVar     = Llb_NonlinFindBestVar( p->dd, bCurrent, p->pAig );

            // get cofactor in the working manager
            bVar     = Cudd_bddIthVar( p->dd, Aig_ObjId(Saig_ManLo(p->pAig,iVar)) );
            bCurrent = Cudd_bddAnd( p->dd, bTemp = bCurrent, Cudd_Not(bVar) );                  Cudd_Ref( bCurrent );
            Cudd_RecursiveDeref( p->dd, bTemp );            
 
            // get cofactor in the global manager
            bVarG    = Cudd_bddIthVar(p->ddG, iVar);
            p->ddG->bFunc2 = Cudd_bddAnd( p->ddG, bTemp = p->ddG->bFunc2, Cudd_Not(bVarG) );    Cudd_Ref( p->ddG->bFunc2 );
            Cudd_RecursiveDeref( p->ddG, bTemp );            
        }

        // compute the next states
        clk3 = clock();
        nBddSize0 = Cudd_DagSize( bCurrent );
        bNext = Llb_NonlinImage( p->pAig, p->vLeaves, p->vRoots, p->pVars2Q, p->dd, bCurrent, 
//            p->pPars->fReorder, p->pPars->fVeryVerbose, p->pOrder, nIters ? p->pPars->nBddMax : ABC_INFINITY );
            p->pPars->fReorder, p->pPars->fVeryVerbose, p->pOrder, ABC_INFINITY );
//        Abc_PrintTime( 1, "Image time", clock() - clk3 );
        if ( bNext == NULL )  // Llb_NonlimImage() consumes reference of bCurrent!!!
        {
            DdNode * bVar, * bVarG;
//          if ( !p->pPars->fSilent )
//              printf( "Reached timeout during image computation (%d seconds).\n",  p->pPars->TimeLimit );
//          p->pPars->iFrame = nIters - 1;
//          return -1;

            printf( "Should never happen!\n" );
 
            // get the frontier in the working manager
            bCurrent = Extra_TransferPermute( p->ddG, p->dd, p->ddG->bFunc2, Vec_IntArray(p->vGlo2Cs) );   Cudd_Ref( bCurrent );

            // find the best variable
            iVar     = Llb_NonlinFindBestVar( p->dd, bCurrent, p->pAig );

            // get cofactor in the working manager
            bVar     = Cudd_bddIthVar( p->dd, Aig_ObjId(Saig_ManLo(p->pAig,iVar)) );
            bCurrent = Cudd_bddAnd( p->dd, bTemp = bCurrent, Cudd_Not(bVar) );                  Cudd_Ref( bCurrent );
            Cudd_RecursiveDeref( p->dd, bTemp );            

            // get cofactor in the global manager
            bVarG    = Cudd_bddIthVar(p->dd, iVar);
            p->ddG->bFunc2 = Cudd_bddAnd( p->ddG, bTemp = p->ddG->bFunc2, Cudd_Not(bVarG) );    Cudd_Ref( p->ddG->bFunc2 );
            Cudd_RecursiveDeref( p->ddG, bTemp );            

//            Cudd_ReduceHeap( p->dd, CUDD_REORDER_SIFT, 1 );
            p->pPars->nBddMax = ABC_INFINITY;
            nIters--;
            continue;
        }
        else
            p->pPars->nBddMax = Limit;

        Cudd_Ref( bNext );
        nBddSize = Cudd_DagSize( bNext );
        p->timeImage += clock() - clk3;

        // transfer to the state manager
        clk3 = clock();
        Cudd_RecursiveDeref( p->ddG, p->ddG->bFunc2 );
        p->ddG->bFunc2 = Extra_TransferPermute( p->dd, p->ddG, bNext, Vec_IntArray(p->vNs2Glo) );    Cudd_Ref( p->ddG->bFunc2 );
        Cudd_RecursiveDeref( p->dd, bNext );
        p->timeTran1 += clock() - clk3;

        // derive new states
        clk3 = clock();
        p->ddG->bFunc2 = Cudd_bddAnd( p->ddG, bTemp = p->ddG->bFunc2, Cudd_Not(p->ddG->bFunc) );     Cudd_Ref( p->ddG->bFunc2 );
        Cudd_RecursiveDeref( p->ddG, bTemp );
        if ( Cudd_IsConstant(p->ddG->bFunc2) )
            break;
        // add to the reached set
        p->ddG->bFunc = Cudd_bddOr( p->ddG, bTemp = p->ddG->bFunc, p->ddG->bFunc2 );                 Cudd_Ref( p->ddG->bFunc );
        Cudd_RecursiveDeref( p->ddG, bTemp );
        p->timeGloba += clock() - clk3;

        // reset permutation
//        RetValue = Cudd_CheckZeroRef( dd );
//        assert( RetValue == 0 );
//        Cudd_ShuffleHeap( dd, pOrder );

        // move new states to the working manager
        clk3 = clock();
        bCurrent = Extra_TransferPermute( p->ddG, p->dd, p->ddG->bFunc2, Vec_IntArray(p->vGlo2Cs) ); Cudd_Ref( bCurrent );
        p->timeTran2 += clock() - clk3;

        // report the results
        if ( p->pPars->fVerbose )
        {
            printf( "I =%3d : ",   nIters );
            printf( "Fr =%7d ",    nBddSize0 );
            printf( "Im =%7d  ",   nBddSize );
            printf( "(%4d %4d)  ", Cudd_ReadReorderings(p->dd),  Cudd_ReadGarbageCollections(p->dd) );
            printf( "Rea =%6d  ",  Cudd_DagSize(p->ddG->bFunc) );
            printf( "(%4d %4d)  ", Cudd_ReadReorderings(p->ddG), Cudd_ReadGarbageCollections(p->ddG) );
            printf( "S =%4d ",     nSuppMax );
            Abc_PrintTime( 1, "T", clock() - clk2 );
        }
/*
        if ( pPars->fVerbose )
        {
            double nMints = Cudd_CountMinterm(ddG, bReached, Saig_ManRegNum(pAig) );
//            Extra_bddPrint( ddG, bReached );printf( "\n" );
            printf( "Reachable states = %.0f. (Ratio = %.4f %%)\n", nMints, 100.0*nMints/pow(2.0, Saig_ManRegNum(pAig)) );
            fflush( stdout ); 
        }
*/

        if ( nIters == p->pPars->nIterMax - 1 )
        {
            if ( !p->pPars->fSilent )
                printf( "Reached limit on the number of timeframes (%d).\n",  p->pPars->nIterMax );
            p->pPars->iFrame = nIters;
            Cudd_RecursiveDeref( p->dd,  bCurrent );       bCurrent = NULL;
            return -1;
        }
//        Llb_NonlinReorder( p->ddG, 1 );
//        Llb_NonlinFindBestVar( p->ddG, bReached, NULL );
    }
    
    // report the stats
    if ( p->pPars->fVerbose )
    {
        double nMints = Cudd_CountMinterm(p->ddG, p->ddG->bFunc, Saig_ManRegNum(p->pAig) );
        if ( nIters >= p->pPars->nIterMax || nBddSize > p->pPars->nBddMax )
            printf( "Reachability analysis is stopped after %d frames.\n", nIters );
        else
            printf( "Reachability analysis completed after %d frames.\n", nIters );
        printf( "Reachable states = %.0f. (Ratio = %.4f %%)\n", nMints, 100.0*nMints/pow(2.0, Saig_ManRegNum(p->pAig)) );
        fflush( stdout ); 
    }
    if ( nIters >= p->pPars->nIterMax || nBddSize > p->pPars->nBddMax )
    {
        if ( !p->pPars->fSilent )
            printf( "Verified only for states reachable in %d frames.  ", nIters );
        return -1; // undecided
    }
    // report
    if ( !p->pPars->fSilent )
        printf( "The miter is proved unreachable after %d iterations.  ", nIters );
    p->pPars->iFrame = nIters - 1;
    Abc_PrintTime( 1, "Time", clock() - clk );
    return 1; // unreachable
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Llb_Mnn_t * Llb_MnnStart( Aig_Man_t * pInit, Aig_Man_t * pAig, Gia_ParLlb_t *  pPars )
{
    Llb_Mnn_t * p;
    Aig_Obj_t * pObj;
    int i;
    p = ABC_CALLOC( Llb_Mnn_t, 1 );
    p->pInit = pInit;
    p->pAig  = pAig;
    p->pPars = pPars;
    p->dd    = Cudd_Init( Aig_ManObjNumMax(pAig), 0, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0 );
    p->ddG   = Cudd_Init( Aig_ManRegNum(pAig),    0, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0 );
    p->ddR   = Cudd_Init( Aig_ManPiNum(pAig),     0, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0 );
    Cudd_AutodynEnable( p->dd,  CUDD_REORDER_SYMM_SIFT );
    Cudd_AutodynEnable( p->ddG, CUDD_REORDER_SYMM_SIFT );
    Cudd_AutodynEnable( p->ddR, CUDD_REORDER_SYMM_SIFT );
    p->vRings = Vec_PtrAlloc( 100 );
    // create leaves
    p->vLeaves = Vec_PtrAlloc( Aig_ManPiNum(pAig) );
    Aig_ManForEachPi( pAig, pObj, i )
        Vec_PtrPush( p->vLeaves, pObj );
    // create roots
    p->vRoots = Vec_PtrAlloc( Aig_ManPoNum(pAig) );
    Saig_ManForEachLi( pAig, pObj, i )
        Vec_PtrPush( p->vRoots, pObj );
    // variables to quantify
    p->pOrder  = ABC_CALLOC( int, Aig_ManObjNumMax(pAig) );
    p->pVars2Q = ABC_CALLOC( int, Aig_ManObjNumMax(pAig) );
    Aig_ManForEachPi( pAig, pObj, i )
        p->pVars2Q[Aig_ObjId(pObj)] = 1;
    Llb_NonlinPrepareVarMap( p ); 
    return p;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Llb_MnnStop( Llb_Mnn_t * p )
{
    DdNode * bTemp;
    int i;
    if ( p->pPars->fVerbose ) 
    {
        p->timeOther = p->timeTotal - p->timeImage - p->timeTran1 - p->timeTran2 - p->timeGloba;
        p->timeReo   = Cudd_ReadReorderingTime(p->dd);
        p->timeReoG  = Cudd_ReadReorderingTime(p->ddG);
        ABC_PRTP( "Image    ", p->timeImage, p->timeTotal );
        ABC_PRTP( "  build  ",    timeBuild, p->timeTotal );
        ABC_PRTP( "  and-ex ",    timeAndEx, p->timeTotal );
        ABC_PRTP( "  other  ",    timeOther, p->timeTotal );
        ABC_PRTP( "Transfer1", p->timeTran1, p->timeTotal );
        ABC_PRTP( "Transfer2", p->timeTran2, p->timeTotal );
        ABC_PRTP( "Global   ", p->timeGloba, p->timeTotal );
        ABC_PRTP( "Other    ", p->timeOther, p->timeTotal );
        ABC_PRTP( "TOTAL    ", p->timeTotal, p->timeTotal );
        ABC_PRTP( "  reo    ", p->timeReo,   p->timeTotal );
        ABC_PRTP( "  reoG   ", p->timeReoG,  p->timeTotal );
    }
    if ( p->ddR->bFunc )
        Cudd_RecursiveDeref( p->ddR, p->ddR->bFunc );
    Vec_PtrForEachEntry( DdNode *, p->vRings, bTemp, i )
        Cudd_RecursiveDeref( p->ddR, bTemp );
    Vec_PtrFree( p->vRings );
    if ( p->ddG->bFunc )
        Cudd_RecursiveDeref( p->ddG, p->ddG->bFunc );
    if ( p->ddG->bFunc2 )
        Cudd_RecursiveDeref( p->ddG, p->ddG->bFunc2 );
//    printf( "manager1\n" );
    Extra_StopManager( p->dd );
//    printf( "manager2\n" );
    Extra_StopManager( p->ddG );
//    printf( "manager3\n" );
    Extra_StopManager( p->ddR );
    Vec_IntFreeP( &p->vCs2Glo );
    Vec_IntFreeP( &p->vNs2Glo );
    Vec_IntFreeP( &p->vGlo2Cs );
    Vec_IntFreeP( &p->vGlo2Ns );
    Vec_PtrFree( p->vLeaves );
    Vec_PtrFree( p->vRoots );
    ABC_FREE( p->pVars2Q );
    ABC_FREE( p->pOrder );
    ABC_FREE( p );
}


/**Function*************************************************************

  Synopsis    [Finds balanced cut.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Llb_NonlinExperiment( Aig_Man_t * pAig, int Num )
{
    Llb_Mnn_t * pMnn;
    Gia_ParLlb_t Pars, * pPars = &Pars;
    Aig_Man_t * p;
    int clk = clock();

    Llb_ManSetDefaultParams( pPars );
    pPars->fVerbose = 1;

    p = Aig_ManDupFlopsOnly( pAig );
//Aig_ManShow( p, 0, NULL );
    Aig_ManPrintStats( pAig );
    Aig_ManPrintStats( p );

    pMnn = Llb_MnnStart( pAig, p, pPars );
    Llb_NonlinReachability( pMnn );
    pMnn->timeTotal = clock() - clk;
    Llb_MnnStop( pMnn );

    Aig_ManStop( p );
}

/**Function*************************************************************

  Synopsis    [Finds balanced cut.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Llb_NonlinCoreReach( Aig_Man_t * pAig, Gia_ParLlb_t * pPars )
{
    Llb_Mnn_t * pMnn;
    Aig_Man_t * p;
    int RetValue = -1;

    p = Aig_ManDupFlopsOnly( pAig );
//Aig_ManShow( p, 0, NULL );
    if ( pPars->fVerbose )
    Aig_ManPrintStats( pAig );
    if ( pPars->fVerbose )
    Aig_ManPrintStats( p );

    if ( !pPars->fSkipReach )
    {
        int clk = clock();
        pMnn = Llb_MnnStart( pAig, p, pPars );
        RetValue = Llb_NonlinReachability( pMnn );
        pMnn->timeTotal = clock() - clk;
        Llb_MnnStop( pMnn );
    }

    Aig_ManStop( p );
    return RetValue;
}

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END

