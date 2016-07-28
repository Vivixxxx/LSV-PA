/**CFile****************************************************************

  FileName    [abcExact.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [Network and node package.]

  Synopsis    [Find minimum size networks with a SAT solver.]

  Author      [Mathias Soeken]

  Affiliation [EPFL]

  Date        [Ver. 1.0. Started - July 15, 2016.]

  Revision    [$Id: abcFanio.c,v 1.00 2005/06/20 00:00:00 alanmi Exp $]

***********************************************************************/

/* This implementation is based on Exercises 477 and 478 in
 * Donald E. Knuth TAOCP Fascicle 6 (Satisfiability) Section 7.2.2.2
 */

#include "base/abc/abc.h"

#include "aig/gia/gia.h"
#include "misc/util/utilTruth.h"
#include "misc/vec/vecInt.h"
#include "misc/vec/vecPtr.h"
#include "proof/cec/cec.h"
#include "sat/bsat/satSolver.h"

ABC_NAMESPACE_IMPL_START


////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

static word s_Truths8[32] = {
    ABC_CONST(0xAAAAAAAAAAAAAAAA), ABC_CONST(0xAAAAAAAAAAAAAAAA), ABC_CONST(0xAAAAAAAAAAAAAAAA), ABC_CONST(0xAAAAAAAAAAAAAAAA),
    ABC_CONST(0xCCCCCCCCCCCCCCCC), ABC_CONST(0xCCCCCCCCCCCCCCCC), ABC_CONST(0xCCCCCCCCCCCCCCCC), ABC_CONST(0xCCCCCCCCCCCCCCCC),
    ABC_CONST(0xF0F0F0F0F0F0F0F0), ABC_CONST(0xF0F0F0F0F0F0F0F0), ABC_CONST(0xF0F0F0F0F0F0F0F0), ABC_CONST(0xF0F0F0F0F0F0F0F0),
    ABC_CONST(0xFF00FF00FF00FF00), ABC_CONST(0xFF00FF00FF00FF00), ABC_CONST(0xFF00FF00FF00FF00), ABC_CONST(0xFF00FF00FF00FF00),
    ABC_CONST(0xFFFF0000FFFF0000), ABC_CONST(0xFFFF0000FFFF0000), ABC_CONST(0xFFFF0000FFFF0000), ABC_CONST(0xFFFF0000FFFF0000),
    ABC_CONST(0xFFFFFFFF00000000), ABC_CONST(0xFFFFFFFF00000000), ABC_CONST(0xFFFFFFFF00000000), ABC_CONST(0xFFFFFFFF00000000),
    ABC_CONST(0x0000000000000000), ABC_CONST(0xFFFFFFFFFFFFFFFF), ABC_CONST(0x0000000000000000), ABC_CONST(0xFFFFFFFFFFFFFFFF),
    ABC_CONST(0x0000000000000000), ABC_CONST(0x0000000000000000), ABC_CONST(0xFFFFFFFFFFFFFFFF), ABC_CONST(0xFFFFFFFFFFFFFFFF)
};

typedef struct Ses_Man_t_ Ses_Man_t;
struct Ses_Man_t_
{
    sat_solver * pSat;          /* SAT solver */

    word *       pSpec;         /* specification */
    int          bSpecInv;      /* remembers whether spec was inverted for normalization */
    int          nSpecVars;     /* number of variables in specification */
    int          nSpecFunc;     /* number of functions to synthesize */
    int          nRows;         /* number of rows in the specification (without 0) */
    int          nMaxDepth;     /* maximum depth (-1 if depth is not constrained) */
    int *        pArrivalTimes; /* arrival times of inputs (NULL if arrival times are ignored) */
    int          nArrivalDelta; /* delta to the original arrival times (arrival times are normalized to have 0 as minimum element) */
    int          nArrivalMax;   /* maximum normalized arrival time */
    int          nBTLimit;      /* conflict limit */
    int          fMakeAIG;      /* create AIG instead of general network */
    int          fVerbose;      /* be verbose */
    int          fVeryVerbose;  /* be very verbose */

    int          nGates;        /* number of gates */

    int          nSimVars;      /* number of simulation vars x(i, t) */
    int          nOutputVars;   /* number of output variables g(h, i) */
    int          nGateVars;     /* number of gate variables f(i, p, q) */
    int          nSelectVars;   /* number of select variables s(i, j, k) */
    int          nDepthVars;    /* number of depth variables d(i, j) */

    int          nOutputOffset; /* offset where output variables start */
    int          nGateOffset;   /* offset where gate variables start */
    int          nSelectOffset; /* offset where select variables start */
    int          nDepthOffset;  /* offset where depth variables start */

    abctime      timeSat;       /* SAT runtime */
    abctime      timeSatSat;    /* SAT runtime (sat instance) */
    abctime      timeSatUnsat;  /* SAT runtime (unsat instance) */
    abctime      timeTotal;     /* all runtime */
};

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

int NormalizeArrivalTimes( int * pArrivalTimes, int nVars, int * maxNormalized )
{
    int * p = pArrivalTimes, * pEnd = pArrivalTimes + nVars;
    int delta = *p;

    while ( ++p < pEnd )
        if ( *p < delta )
            delta = *p;

    *maxNormalized = 0;
    p = pArrivalTimes;
    while ( p < pEnd )
    {
        *p -= delta;
        if ( *p > *maxNormalized )
            *maxNormalized = *p;
        ++p;
    }

    *maxNormalized += 1;

    return delta;
}

static inline Ses_Man_t * Ses_ManAlloc( word * pTruth, int nVars, int nFunc, int nMaxDepth, int * pArrivalTimes, int fMakeAIG, int fVerbose )
{
    int h, i;

    Ses_Man_t * p = ABC_CALLOC( Ses_Man_t, 1 );
    p->pSat       = NULL;
    p->bSpecInv   = 0;
    for ( h = 0; h < nFunc; ++h )
        if ( pTruth[h << 2] & 1 )
        {
            for ( i = 0; i < 4; ++i )
                pTruth[(h << 2) + i] = ~pTruth[(h << 2) + i];
            p->bSpecInv |= ( 1 << h );
        }
    p->pSpec         = pTruth;
    p->nSpecVars     = nVars;
    p->nSpecFunc     = nFunc;
    p->nRows         = ( 1 << nVars ) - 1;
    p->nMaxDepth     = nMaxDepth;
    p->pArrivalTimes = nMaxDepth >= 0 ? pArrivalTimes : NULL;
    if ( p->pArrivalTimes )
        p->nArrivalDelta = NormalizeArrivalTimes( p->pArrivalTimes, nVars, &p->nArrivalMax );
    else
        p->nArrivalDelta = p->nArrivalMax = 0;
    p->fMakeAIG      = fMakeAIG;
    p->nBTLimit      = nMaxDepth >= 0 ? 50000 : 0;
    p->fVerbose      = fVerbose;
    p->fVeryVerbose  = 0;

    return p;
}

static inline void Ses_ManClean( Ses_Man_t * pSes )
{
    int h, i;
    for ( h = 0; h < pSes->nSpecFunc; ++h )
        if ( ( pSes->bSpecInv >> h ) & 1 )
            for ( i = 0; i < 4; ++i )
                pSes->pSpec[(h << 2) + i] = ~( pSes->pSpec[(h << 2) + i] );

    if ( pSes->pArrivalTimes )
        for ( i = 0; i < pSes->nSpecVars; ++i )
            pSes->pArrivalTimes[i] += pSes->nArrivalDelta;

    if ( pSes->pSat )
        sat_solver_delete( pSes->pSat );

    ABC_FREE( pSes );
}

/**Function*************************************************************

  Synopsis    [Access variables based on indexes.]

***********************************************************************/
static inline int Ses_ManSimVar( Ses_Man_t * pSes, int i, int t )
{
    assert( i < pSes->nGates );
    assert( t < pSes->nRows );

    return pSes->nRows * i + t;
}

static inline int Ses_ManOutputVar( Ses_Man_t * pSes, int h, int i )
{
    assert( h < pSes->nSpecFunc );
    assert( i < pSes->nGates );

    return pSes->nOutputOffset + pSes->nGates * h + i;
}

static inline int Ses_ManGateVar( Ses_Man_t * pSes, int i, int p, int q )
{
    assert( i < pSes->nGates );
    assert( p < 2 );
    assert( q < 2 );
    assert( p > 0 || q > 0 );

    return pSes->nGateOffset + i * 3 + ( p << 1 ) + q - 1;
}

static inline int Ses_ManSelectVar( Ses_Man_t * pSes, int i, int j, int k )
{
    int a;
    int offset;

    assert( i < pSes->nGates );
    assert( k < pSes->nSpecVars + i );
    assert( j < k );

    offset = pSes->nSelectOffset;
    for ( a = pSes->nSpecVars; a < pSes->nSpecVars + i; ++a )
        offset += a * ( a - 1 ) / 2;

    return offset + ( -j * ( 1 + j - 2 * ( pSes->nSpecVars + i ) ) ) / 2 + ( k - j - 1 );
}

static inline int Ses_ManDepthVar( Ses_Man_t * pSes, int i, int j )
{
    assert( i < pSes->nGates );
    assert( j <= pSes->nArrivalMax + i );

    return pSes->nDepthOffset + i * pSes->nArrivalMax + ( ( i * ( i + 1 ) ) / 2 ) + j;
}

/**Function*************************************************************

  Synopsis    [Setup variables to find network with nGates gates.]

***********************************************************************/
static void Ses_ManCreateVars( Ses_Man_t * pSes, int nGates )
{
    int i;

    if ( pSes->fVerbose )
    {
        printf( "create variables for network with %d functions over %d variables and %d gates\n", pSes->nSpecFunc, pSes->nSpecVars, nGates );
    }

    pSes->nGates      = nGates;
    pSes->nSimVars    = nGates * pSes->nRows;
    pSes->nOutputVars = pSes->nSpecFunc * nGates;
    pSes->nGateVars   = nGates * 3;
    pSes->nSelectVars = 0;
    for ( i = pSes->nSpecVars; i < pSes->nSpecVars + nGates; ++i )
        pSes->nSelectVars += ( i * ( i - 1 ) ) / 2;
    pSes->nDepthVars = pSes->nMaxDepth > 0 ? nGates * pSes->nArrivalMax + ( nGates * ( nGates + 1 ) ) / 2 : 0;

    pSes->nOutputOffset = pSes->nSimVars;
    pSes->nGateOffset   = pSes->nSimVars + pSes->nOutputVars;
    pSes->nSelectOffset = pSes->nSimVars + pSes->nOutputVars + pSes->nGateVars;
    pSes->nDepthOffset  = pSes->nSimVars + pSes->nOutputVars + pSes->nGateVars + pSes->nSelectVars;

    if ( pSes->pSat )
        sat_solver_delete( pSes->pSat );
    pSes->pSat = sat_solver_new();
    sat_solver_setnvars( pSes->pSat, pSes->nSimVars + pSes->nOutputVars + pSes->nGateVars + pSes->nSelectVars + pSes->nDepthVars );
}

/**Function*************************************************************

  Synopsis    [Create clauses.]

***********************************************************************/
static inline void Ses_ManCreateMainClause( Ses_Man_t * pSes, int t, int i, int j, int k, int a, int b, int c )
{
    int pLits[5], ctr = 0, value;

    pLits[ctr++] = Abc_Var2Lit( Ses_ManSelectVar( pSes, i, j, k ), 1 );
    pLits[ctr++] = Abc_Var2Lit( Ses_ManSimVar( pSes, i, t ), a );

    if ( j < pSes->nSpecVars )
    {
        if ( Abc_TtGetBit( s_Truths8 + ( j << 2 ), t + 1 ) != b ) /* 1 in clause, we can omit the clause */
            return;
    }
    else
        pLits[ctr++] = Abc_Var2Lit( Ses_ManSimVar( pSes, j - pSes->nSpecVars, t ), b );

    if ( k < pSes->nSpecVars )
    {
        if ( Abc_TtGetBit( s_Truths8 + ( k << 2 ), t + 1 ) != c ) /* 1 in clause, we can omit the clause */
            return;
    }
    else
        pLits[ctr++] = Abc_Var2Lit( Ses_ManSimVar( pSes, k - pSes->nSpecVars, t ), c );

    if ( b > 0 || c > 0 )
        pLits[ctr++] = Abc_Var2Lit( Ses_ManGateVar( pSes, i, b, c ), 1 - a );

    value = sat_solver_addclause( pSes->pSat, pLits, pLits + ctr );
    assert( value );
}

static void Ses_ManCreateClauses( Ses_Man_t * pSes )
{
    extern int Extra_TruthVarsSymm( unsigned * pTruth, int nVars, int iVar0, int iVar1 );

    int h, i, j, k, t, ii, jj, kk, p, q, d;
    int pLits[3];
    Vec_Int_t * vLits;

    for ( t = 0; t < pSes->nRows; ++t )
        for ( i = 0; i < pSes->nGates; ++i )
        {
            /* main clauses */
            for ( j = 0; j < pSes->nSpecVars + i; ++j )
                for ( k = j + 1; k < pSes->nSpecVars + i; ++k )
                {
                    Ses_ManCreateMainClause( pSes, t, i, j, k, 0, 0, 1 );
                    Ses_ManCreateMainClause( pSes, t, i, j, k, 0, 1, 0 );
                    Ses_ManCreateMainClause( pSes, t, i, j, k, 0, 1, 1 );
                    Ses_ManCreateMainClause( pSes, t, i, j, k, 1, 0, 0 );
                    Ses_ManCreateMainClause( pSes, t, i, j, k, 1, 0, 1 );
                    Ses_ManCreateMainClause( pSes, t, i, j, k, 1, 1, 0 );
                    Ses_ManCreateMainClause( pSes, t, i, j, k, 1, 1, 1 );
                }

            /* output clauses */
            for ( h = 0; h < pSes->nSpecFunc; ++h )
            {
                pLits[0] = Abc_Var2Lit( Ses_ManOutputVar( pSes, h, i ), 1 );
                pLits[1] = Abc_Var2Lit( Ses_ManSimVar( pSes, i, t ), 1 - Abc_TtGetBit( &pSes->pSpec[h << 2], t + 1 ) );
                assert( sat_solver_addclause( pSes->pSat, pLits, pLits + 2 ) );
            }
        }

    /* some output is selected */
    for ( h = 0; h < pSes->nSpecFunc; ++h )
    {
        vLits = Vec_IntAlloc( pSes->nGates );
        for ( i = 0; i < pSes->nGates; ++i )
            Vec_IntPush( vLits, Abc_Var2Lit( Ses_ManOutputVar( pSes, h, i ), 0 ) );
        assert( sat_solver_addclause( pSes->pSat, Vec_IntArray( vLits ), Vec_IntLimit( vLits ) ) );
        Vec_IntFree( vLits );
    }

    /* each gate has two operands */
    for ( i = 0; i < pSes->nGates; ++i )
    {
        vLits = Vec_IntAlloc( ( ( pSes->nSpecVars + i ) * ( pSes->nSpecVars + i - 1 ) ) / 2 );
        for ( j = 0; j < pSes->nSpecVars + i; ++j )
            for ( k = j + 1; k < pSes->nSpecVars + i; ++k )
                Vec_IntPush( vLits, Abc_Var2Lit( Ses_ManSelectVar( pSes, i, j, k ), 0 ) );
        assert( sat_solver_addclause( pSes->pSat, Vec_IntArray( vLits ), Vec_IntLimit( vLits ) ) );
        Vec_IntFree( vLits );
    }

    /* only AIG */
    if ( pSes->fMakeAIG )
    {
        for ( i = 0; i < pSes->nGates; ++i )
        {
            /* not 2 ones */
            pLits[0] = Abc_Var2Lit( Ses_ManGateVar( pSes, i, 0, 1 ), 1 );
            pLits[1] = Abc_Var2Lit( Ses_ManGateVar( pSes, i, 1, 0 ), 1 );
            pLits[2] = Abc_Var2Lit( Ses_ManGateVar( pSes, i, 1, 1 ), 0 );
            assert( sat_solver_addclause( pSes->pSat, pLits, pLits + 3 ) );

            pLits[0] = Abc_Var2Lit( Ses_ManGateVar( pSes, i, 0, 1 ), 1 );
            pLits[1] = Abc_Var2Lit( Ses_ManGateVar( pSes, i, 1, 0 ), 0 );
            pLits[2] = Abc_Var2Lit( Ses_ManGateVar( pSes, i, 1, 1 ), 1 );
            assert( sat_solver_addclause( pSes->pSat, pLits, pLits + 3 ) );

            pLits[0] = Abc_Var2Lit( Ses_ManGateVar( pSes, i, 0, 1 ), 0 );
            pLits[1] = Abc_Var2Lit( Ses_ManGateVar( pSes, i, 1, 0 ), 1 );
            pLits[2] = Abc_Var2Lit( Ses_ManGateVar( pSes, i, 1, 1 ), 1 );
            assert( sat_solver_addclause( pSes->pSat, pLits, pLits + 3 ) );
        }
    }

    /* EXTRA clauses: use gate i at least once */
    for ( i = 0; i < pSes->nGates; ++i )
    {
        vLits = Vec_IntAlloc( 0 );
        for ( h = 0; h < pSes->nSpecFunc; ++h )
            Vec_IntPush( vLits, Abc_Var2Lit( Ses_ManOutputVar( pSes, h, i ), 0 ) );
        for ( ii = i + 1; ii < pSes->nGates; ++ii )
        {
            for ( j = 0; j < pSes->nSpecVars + i; ++j )
                Vec_IntPush( vLits, Abc_Var2Lit( Ses_ManSelectVar( pSes, ii, j, pSes->nSpecVars + i ), 0 ) );
            for ( j = pSes->nSpecVars + i + 1; j < pSes->nSpecVars + ii; ++j )
                Vec_IntPush( vLits, Abc_Var2Lit( Ses_ManSelectVar( pSes, ii, pSes->nSpecVars + i, j ), 0 ) );
        }
        assert( sat_solver_addclause( pSes->pSat, Vec_IntArray( vLits ), Vec_IntLimit( vLits ) ) );
        Vec_IntFree( vLits );
    }

    /* EXTRA clauses: co-lexicographic order */
    for ( i = 0; i < pSes->nGates - 1; ++i )
    {
        for ( k = 2; k < pSes->nSpecVars + i; ++k )
        {
            for ( j = 1; j < k; ++j )
                for ( jj = 0; jj < j; ++jj )
                {
                    pLits[0] = Abc_Var2Lit( Ses_ManSelectVar( pSes, i, j, k ), 1 );
                    pLits[1] = Abc_Var2Lit( Ses_ManSelectVar( pSes, i + 1, jj, k ), 1 );
                }

            for ( j = 0; j < k; ++j )
                for ( kk = 1; kk < k; ++kk )
                    for ( jj = 0; jj < kk; ++jj )
                    {
                        pLits[0] = Abc_Var2Lit( Ses_ManSelectVar( pSes, i, j, k ), 1 );
                        pLits[1] = Abc_Var2Lit( Ses_ManSelectVar( pSes, i + 1, jj, kk ), 1 );
                    }
        }
    }

    /* EXTRA clauses: symmetric variables */
    if ( pSes->nSpecFunc == 1 ) /* only check if there is one output function */
        for ( q = 1; q < pSes->nSpecVars; ++q )
            for ( p = 0; p < q; ++p )
                if ( Extra_TruthVarsSymm( (unsigned*)( &pSes->pSpec[h << 2] ), pSes->nSpecVars, p, q ) )
                {
                    if ( pSes->fVeryVerbose )
                        printf( "variables %d and %d are symmetric\n", p, q );
                    for ( i = 0; i < pSes->nGates; ++i )
                        for ( j = 0; j < q; ++j )
                        {
                            if ( j == p ) continue;

                            vLits = Vec_IntAlloc( 0 );
                            Vec_IntPush( vLits, Abc_Var2Lit( Ses_ManSelectVar( pSes, i, j, q ), 1 ) );
                            for ( ii = 0; ii < i; ++ii )
                                for ( kk = 1; kk < pSes->nSpecVars + ii; ++kk )
                                    for ( jj = 0; jj < kk; ++jj )
                                        if ( jj == p || kk == p )
                                            Vec_IntPush( vLits, Abc_Var2Lit( Ses_ManSelectVar( pSes, ii, jj, kk ), 0 ) );
                            assert( sat_solver_addclause( pSes->pSat, Vec_IntArray( vLits ), Vec_IntLimit( vLits ) ) );
                            Vec_IntFree( vLits );
                        }
                }

    /* DEPTH clauses */
    if ( pSes->nMaxDepth > 0 )
    {
        for ( i = 0; i < pSes->nGates; ++i )
        {
            /* propagate depths from children */
            for ( k = 1; k < i; ++k )
                for ( j = 0; j < k; ++j )
                {
                    pLits[0] = Abc_Var2Lit( Ses_ManSelectVar( pSes, i, pSes->nSpecVars + j, pSes->nSpecVars + k ), 1 );
                    for ( jj = 0; jj <= pSes->nArrivalMax + j; ++jj )
                    {
                        pLits[1] = Abc_Var2Lit( Ses_ManDepthVar( pSes, j, jj ), 1 );
                        pLits[2] = Abc_Var2Lit( Ses_ManDepthVar( pSes, i, jj + 1 ), 0 );
                        assert( sat_solver_addclause( pSes->pSat, pLits, pLits + 3 ) );
                    }
                }

            for ( k = 0; k < i; ++k )
                for ( j = 0; j < pSes->nSpecVars + k; ++j )
                {
                    pLits[0] = Abc_Var2Lit( Ses_ManSelectVar( pSes, i, j, pSes->nSpecVars + k ), 1 );
                    for ( kk = 0; kk <= pSes->nArrivalMax + k; ++kk )
                    {
                        pLits[1] = Abc_Var2Lit( Ses_ManDepthVar( pSes, k, kk ), 1 );
                        pLits[2] = Abc_Var2Lit( Ses_ManDepthVar( pSes, i, kk + 1 ), 0 );
                        assert( sat_solver_addclause( pSes->pSat, pLits, pLits + 3 ) );
                    }
                }

            /* propagate depths from arrival times at PIs */
            if ( pSes->pArrivalTimes )
            {
                for ( k = 1; k < pSes->nSpecVars + i; ++k )
                    for ( j = 0; j < ( ( k < pSes->nSpecVars ) ? k : pSes->nSpecVars ); ++j )
                    {
                        d = pSes->pArrivalTimes[j];
                        if ( k < pSes->nSpecVars && pSes->pArrivalTimes[k] > d )
                            d = pSes->pArrivalTimes[k];

                        pLits[0] = Abc_Var2Lit( Ses_ManSelectVar( pSes, i, j, k ), 1 );
                        pLits[1] = Abc_Var2Lit( Ses_ManDepthVar( pSes, i, d + 1 ), 0 );
                        assert( sat_solver_addclause( pSes->pSat, pLits, pLits + 2 ) );
                    }
            }
            else
            {
                /* arrival times are 0 */
                pLits[0] = Abc_Var2Lit( Ses_ManDepthVar( pSes, i, 0 ), 0 );
                assert( sat_solver_addclause( pSes->pSat, pLits, pLits + 1 ) );
            }

            /* reverse order encoding of depth variables */
            for ( j = 1; j <= pSes->nArrivalMax + i; ++j )
            {
                pLits[0] = Abc_Var2Lit( Ses_ManDepthVar( pSes, i, j ), 1 );
                pLits[1] = Abc_Var2Lit( Ses_ManDepthVar( pSes, i, j - 1 ), 0 );
                assert( sat_solver_addclause( pSes->pSat, pLits, pLits + 2 ) );
            }

            /* constrain maximum depth */
            if ( pSes->nMaxDepth < pSes->nArrivalMax + i )
                for ( h = 0; h < pSes->nSpecFunc; ++h )
                {
                    pLits[0] = Abc_Var2Lit( Ses_ManOutputVar( pSes, h, i ), 1 );
                    pLits[1] = Abc_Var2Lit( Ses_ManDepthVar( pSes, i, pSes->nMaxDepth ), 1 );
                    assert( sat_solver_addclause( pSes->pSat, pLits, pLits + 2 ) );
                }
        }
    }
}

/**Function*************************************************************

  Synopsis    [Solve.]

***********************************************************************/
static inline int Ses_ManSolve( Ses_Man_t * pSes )
{
    int status;
    abctime timeStart, timeDelta;

    if ( pSes->fVeryVerbose )
    {
        printf( "solve SAT instance with %d clauses and %d variables\n", sat_solver_nclauses( pSes->pSat ), sat_solver_nvars( pSes->pSat ) );
    }

    timeStart = Abc_Clock();
    status = sat_solver_solve( pSes->pSat, NULL, NULL, pSes->nBTLimit, 0, 0, 0 );
    timeDelta = Abc_Clock() - timeStart;

    pSes->timeSat += timeDelta;

    if ( status == l_True )
    {
        pSes->timeSatSat += timeDelta;
        return 1;
    }
    else if ( status == l_False )
    {
        pSes->timeSatUnsat += timeDelta;
        return 0;
    }
    else
    {
        if ( pSes->fVerbose )
        {
            printf( "resource limit reached\n" );
        }
        return 2;
    }
}

/**Function*************************************************************

  Synopsis    [Extract solution.]

***********************************************************************/
static Abc_Ntk_t * Ses_ManExtractNtk( Ses_Man_t * pSes )
{
    int h, i, j, k;
    Abc_Ntk_t * pNtk;
    Abc_Obj_t * pObj;
    Vec_Ptr_t * pGates, * vNames;
    char pGateTruth[5];
    char * pSopCover;

    pNtk = Abc_NtkAlloc( ABC_NTK_LOGIC, ABC_FUNC_SOP, 1 );
    pNtk->pName = Extra_UtilStrsav( "exact" );
    pGates = Vec_PtrAlloc( pSes->nSpecVars + pSes->nGates );
    pGateTruth[3] = '0';
    pGateTruth[4] = '\0';
    vNames = Abc_NodeGetFakeNames( pSes->nSpecVars + pSes->nSpecFunc );

    /* primary inputs */
    Vec_PtrPush( pNtk->vObjs, NULL );
    for ( i = 0; i < pSes->nSpecVars; ++i )
    {
        pObj = Abc_NtkCreatePi( pNtk );
        Abc_ObjAssignName( pObj, (char*)Vec_PtrEntry( vNames, i ), NULL );
        Vec_PtrPush( pGates, pObj );
    }

    /* gates */
    for ( i = 0; i < pSes->nGates; ++i )
    {
        pGateTruth[2] = '0' + sat_solver_var_value( pSes->pSat, Ses_ManGateVar( pSes, i, 0, 1 ) );
        pGateTruth[1] = '0' + sat_solver_var_value( pSes->pSat, Ses_ManGateVar( pSes, i, 1, 0 ) );
        pGateTruth[0] = '0' + sat_solver_var_value( pSes->pSat, Ses_ManGateVar( pSes, i, 1, 1 ) );

        if ( pSes->fVeryVerbose )
        {
            printf( "gate %d has truth table %s", pSes->nSpecVars + i, pGateTruth );
        }

        pSopCover = Abc_SopFromTruthBin( pGateTruth );
        pObj = Abc_NtkCreateNode( pNtk );
        pObj->pData = Abc_SopRegister( (Mem_Flex_t*)pNtk->pManFunc, pSopCover );
        Vec_PtrPush( pGates, pObj );
        ABC_FREE( pSopCover );

        for ( k = 0; k < pSes->nSpecVars + i; ++k )
            for ( j = 0; j < k; ++j )
                if ( sat_solver_var_value( pSes->pSat, Ses_ManSelectVar( pSes, i, j, k ) ) )
                {
                    if ( pSes->fVeryVerbose )
                        printf( " with children %d and %d", j, k );
                    Abc_ObjAddFanin( pObj, (Abc_Obj_t *)Vec_PtrEntry( pGates, j ) );
                    Abc_ObjAddFanin( pObj, (Abc_Obj_t *)Vec_PtrEntry( pGates, k ) );
                    break;
                }

        if ( pSes->fVeryVerbose )
        {
            if ( pSes->nMaxDepth > 0 )
            {
                printf( " and depth vector " );
                for ( j = 0; j <= pSes->nArrivalMax + i; ++j )
                    printf( "%d", sat_solver_var_value( pSes->pSat, Ses_ManDepthVar( pSes, i, j ) ) );
            }
            printf( "\n" );
        }
    }

    /* outputs */
    for ( h = 0; h < pSes->nSpecFunc; ++h )
    {
        pObj = Abc_NtkCreatePo( pNtk );
        Abc_ObjAssignName( pObj, (char*)Vec_PtrEntry( vNames, pSes->nSpecVars + h ), NULL );
        for ( i = 0; i < pSes->nGates; ++i )
        {
            if ( sat_solver_var_value( pSes->pSat, Ses_ManOutputVar( pSes, h, i ) ) )
            {
                if ( pSes->fVeryVerbose )
                    printf( "output %d points to gate %d", h, pSes->nSpecVars + i );
                /* if output has been inverted, we need to add an inverter */
                if ( ( pSes->bSpecInv >> h ) & 1 )
                {
                    Abc_ObjAddFanin( pObj, Abc_NtkCreateNodeInv( pNtk, (Abc_Obj_t *)Vec_PtrEntry( pGates, pSes->nSpecVars + i ) ) );
                    if ( pSes->fVeryVerbose )
                        printf( " and needs to be inverted" );
                }
                else
                    Abc_ObjAddFanin( pObj, (Abc_Obj_t *)Vec_PtrEntry( pGates, pSes->nSpecVars + i ) );

                if ( pSes->fVeryVerbose )
                    printf( "\n" );
            }
        }
    }
    Abc_NodeFreeNames( vNames );

    Vec_PtrFree( pGates );

    if ( !Abc_NtkCheck( pNtk ) )
        printf( "Ses_ManExtractSolution(): Network check has failed.\n" );


    return pNtk;
}

static Gia_Man_t * Ses_ManExtractGia( Ses_Man_t * pSes )
{
    int h, i, j, k;
    Gia_Man_t * pGia;
    Vec_Int_t * pGates;
    Vec_Ptr_t * vNames;
    int nObj, nChild1, nChild2, fChild1Comp, fChild2Comp;

    pGia = Gia_ManStart( pSes->nSpecVars + pSes->nGates + pSes->nSpecFunc + 1 );
    pGia->nConstrs = 0;
    pGia->pName = Extra_UtilStrsav( "exact" );

    pGates = Vec_IntAlloc( pSes->nSpecVars + pSes->nGates );
    vNames = Abc_NodeGetFakeNames( pSes->nSpecVars + pSes->nSpecFunc );

    /* primary inputs */
    pGia->vNamesIn = Vec_PtrStart( pSes->nSpecVars );
    for ( i = 0; i < pSes->nSpecVars; ++i )
    {
        nObj = Gia_ManAppendCi( pGia );
        Vec_IntPush( pGates, nObj );
        Vec_PtrSetEntry( pGia->vNamesIn, i, Extra_UtilStrsav( Vec_PtrEntry( vNames, i ) ) );
    }

    /* gates */
    for ( i = 0; i < pSes->nGates; ++i )
    {
        for ( k = 0; k < pSes->nSpecVars + i; ++k )
            for ( j = 0; j < k; ++j )
                if ( sat_solver_var_value( pSes->pSat, Ses_ManSelectVar( pSes, i, j, k ) ) )
                {
                    nChild1 = Vec_IntEntry( pGates, j );
                    nChild2 = Vec_IntEntry( pGates, k );
                    fChild1Comp = fChild2Comp = 0;

                    if ( sat_solver_var_value( pSes->pSat, Ses_ManGateVar( pSes, i, 0, 1 ) ) )
                    {
                        nChild1 = Abc_LitNot( nChild1 );
                        fChild1Comp = 1;
                    }
                    if ( sat_solver_var_value( pSes->pSat, Ses_ManGateVar( pSes, i, 1, 0 ) ) )
                    {
                        nChild2 = Abc_LitNot( nChild2 );
                        fChild2Comp = 1;
                    }
                    nObj = Gia_ManAppendAnd( pGia, nChild1, nChild2 );
                    if ( fChild1Comp && fChild2Comp )
                    {
                        assert( sat_solver_var_value( pSes->pSat, Ses_ManGateVar( pSes, i, 1, 1 ) ) );
                        nObj = Abc_LitNot( nObj );
                    }

                    Vec_IntPush( pGates, nObj );

                    break;
                }
    }

    /* outputs */
    pGia->vNamesOut = Vec_PtrStart( pSes->nSpecFunc );
    for ( h = 0; h < pSes->nSpecFunc; ++h )
    {
        for ( i = 0; i < pSes->nGates; ++i )
        {
            if ( sat_solver_var_value( pSes->pSat, Ses_ManOutputVar( pSes, h, i ) ) )
            {
                nObj = Vec_IntEntry( pGates, pSes->nSpecVars + i );
                /* if output has been inverted, we need to add an inverter */
                if ( ( pSes->bSpecInv >> h ) & 1 )
                    nObj = Abc_LitNot( nObj );
                Gia_ManAppendCo( pGia, nObj );
                Vec_PtrSetEntry( pGia->vNamesOut, h, Extra_UtilStrsav( Vec_PtrEntry( vNames, pSes->nSpecVars + h ) ) );
            }
        }
    }
    Abc_NodeFreeNames( vNames );

    Vec_IntFree( pGates );

    return pGia;
}

/**Function*************************************************************

  Synopsis    [Debug.]

***********************************************************************/
static void Ses_ManPrintRuntime( Ses_Man_t * pSes )
{
    printf( "Runtime breakdown:\n" );
    ABC_PRTP( "Sat   ", pSes->timeSat,      pSes->timeTotal );
    ABC_PRTP( " Sat  ", pSes->timeSatSat,   pSes->timeTotal );
    ABC_PRTP( " Unsat", pSes->timeSatUnsat, pSes->timeTotal );
    ABC_PRTP( "ALL   ", pSes->timeTotal,    pSes->timeTotal );
}

static inline void Ses_ManPrintFuncs( Ses_Man_t * pSes )
{
    int h;

    printf( "find optimum circuit for %d %d-variable functions:\n", pSes->nSpecFunc, pSes->nSpecVars );
    for ( h = 0; h < pSes->nSpecFunc; ++h )
    {
        printf( "  func %d: ", h + 1 );
        Abc_TtPrintHexRev( stdout, &pSes->pSpec[h >> 2], pSes->nSpecVars );
        printf( "\n" );
    }
}

static inline void Ses_ManPrintVars( Ses_Man_t * pSes )
{
    int h, i, j, k, p, q, t;

    for ( i = 0; i < pSes->nGates; ++i )
        for ( t = 0; t < pSes->nRows; ++t )
            printf( "x(%d, %d) : %d\n", i, t, Ses_ManSimVar( pSes, i, t ) );

    for ( h = 0; h < pSes->nSpecFunc; ++h )
        for ( i = 0; i < pSes->nGates; ++i )
            printf( "h(%d, %d) : %d\n", h, i, Ses_ManOutputVar( pSes, h, i ) );

    for ( i = 0; i < pSes->nGates; ++i )
        for ( p = 0; p <= 1; ++p )
            for ( q = 0; q <= 1; ++ q)
            {
                if ( p == 0 && q == 0 ) { continue; }
                printf( "f(%d, %d, %d) : %d\n", i, p, q, Ses_ManGateVar( pSes, i, p, q ) );
            }

    for ( i = 0; i < pSes->nGates; ++i )
        for ( j = 0; j < pSes->nSpecVars + i; ++j )
            for ( k = j + 1; k < pSes->nSpecVars + i; ++k )
                printf( "s(%d, %d, %d) : %d\n", i, j, k, Ses_ManSelectVar( pSes, i, j, k ) );

    if ( pSes->nMaxDepth > 0 )
        for ( i = 0; i < pSes->nGates; ++i )
            for ( j = 0; j <= i; ++j )
                printf( "d(%d, %d) : %d\n", i, j, Ses_ManDepthVar( pSes, i, j ) );

}

/**Function*************************************************************

  Synopsis    [Synthesis algorithm.]

***********************************************************************/
static int Ses_ManFindMinimumSize( Ses_Man_t * pSes )
{
    int nGates = 0;

    while ( true )
    {
        ++nGates;

        /* give up if number of gates is impossible for given depth */
        if ( pSes->nMaxDepth != -1 && nGates >= ( 1 << pSes->nMaxDepth ) )
            return 0;

        Ses_ManCreateVars( pSes, nGates );
        Ses_ManCreateClauses( pSes );

        switch ( Ses_ManSolve( pSes ) )
        {
        case 1: return 1; /* SAT */
        case 2: return 0; /* resource limit */
        }
    }

    return 0;
}

/**Function*************************************************************

  Synopsis    [Find minimum size networks with a SAT solver.]

  Description [If nMaxDepth is -1, then depth constraints are ignored.
               If nMaxDepth is not -1, one can set pArrivalTimes which should have the length of nVars.
               One can ignore pArrivalTimes by setting it to NULL.]

  SideEffects []

  SeeAlso     []

***********************************************************************/
Abc_Ntk_t * Abc_NtkFindExact( word * pTruth, int nVars, int nFunc, int nMaxDepth, int * pArrivalTimes, int fVerbose )
{
    Ses_Man_t * pSes;
    Abc_Ntk_t * pNtk = NULL;
    abctime timeStart;

    /* some checks */
    assert( nVars >= 2 && nVars <= 8 );

    timeStart = Abc_Clock();

    pSes = Ses_ManAlloc( pTruth, nVars, nFunc, nMaxDepth, pArrivalTimes, 0, fVerbose );
    if ( fVerbose )
        Ses_ManPrintFuncs( pSes );

    if ( Ses_ManFindMinimumSize( pSes ) )
        pNtk = Ses_ManExtractNtk( pSes );

    pSes->timeTotal = Abc_Clock() - timeStart;

    if ( fVerbose )
        Ses_ManPrintRuntime( pSes );

    /* cleanup */
    Ses_ManClean( pSes );

    return pNtk;
}

Gia_Man_t * Gia_ManFindExact( word * pTruth, int nVars, int nFunc, int nMaxDepth, int * pArrivalTimes, int fVerbose )
{
    Ses_Man_t * pSes;
    Gia_Man_t * pGia = NULL;
    abctime timeStart;

    /* some checks */
    assert( nVars >= 2 && nVars <= 8 );

    timeStart = Abc_Clock();

    pSes = Ses_ManAlloc( pTruth, nVars, nFunc, nMaxDepth, pArrivalTimes, 1, fVerbose );
    if ( fVerbose )
        Ses_ManPrintFuncs( pSes );

    if ( Ses_ManFindMinimumSize( pSes ) )
        pGia = Ses_ManExtractGia( pSes );

    pSes->timeTotal = Abc_Clock() - timeStart;

    if ( fVerbose )
        Ses_ManPrintRuntime( pSes );

    /* cleanup */
    Ses_ManClean( pSes );

    return pGia;
}

/**Function*************************************************************

  Synopsis    [Some test cases.]

***********************************************************************/

Abc_Ntk_t * Abc_NtkFromTruthTable( word * pTruth, int nVars )
{
    Abc_Ntk_t * pNtk;
    Mem_Flex_t * pMan;
    char * pSopCover;

    pMan = Mem_FlexStart();
    pSopCover = Abc_SopCreateFromTruth( pMan, nVars, (unsigned*)pTruth );
    pNtk = Abc_NtkCreateWithNode( pSopCover );
    Abc_NtkShortNames( pNtk );
    Mem_FlexStop( pMan, 0 );

    return pNtk;
}

void Abc_ExactTestSingleOutput( int fVerbose )
{
    extern void Abc_NtkCecSat( Abc_Ntk_t * pNtk1, Abc_Ntk_t * pNtk2, int nConfLimit, int nInsLimit );

    word pTruth[4] = {0xcafe, 0, 0, 0};
    Abc_Ntk_t * pNtk, * pNtk2, * pNtk3, * pNtk4;
    int pArrivalTimes[4] = {6, 2, 8, 5};

    pNtk = Abc_NtkFromTruthTable( pTruth, 4 );

    pNtk2 = Abc_NtkFindExact( pTruth, 4, 1, -1, NULL, fVerbose );
    Abc_NtkShortNames( pNtk2 );
    Abc_NtkCecSat( pNtk, pNtk2, 10000, 0 );
    assert( pNtk2 );
    assert( Abc_NtkNodeNum( pNtk2 ) == 6 );
    Abc_NtkDelete( pNtk2 );

    pNtk3 = Abc_NtkFindExact( pTruth, 4, 1, 3, NULL, fVerbose );
    Abc_NtkShortNames( pNtk3 );
    Abc_NtkCecSat( pNtk, pNtk3, 10000, 0 );
    assert( pNtk3 );
    assert( Abc_NtkLevel( pNtk3 ) <= 3 );
    Abc_NtkDelete( pNtk3 );

    pNtk4 = Abc_NtkFindExact( pTruth, 4, 1, 9, pArrivalTimes, fVerbose );
    Abc_NtkShortNames( pNtk4 );
    Abc_NtkCecSat( pNtk, pNtk4, 10000, 0 );
    assert( pNtk4 );
    assert( Abc_NtkLevel( pNtk4 ) <= 9 );
    Abc_NtkDelete( pNtk4 );

    assert( !Abc_NtkFindExact( pTruth, 4, 1, 2, NULL, fVerbose ) );

    assert( !Abc_NtkFindExact( pTruth, 4, 1, 8, pArrivalTimes, fVerbose ) );

    Abc_NtkDelete( pNtk );
}

void Abc_ExactTestSingleOutputAIG( int fVerbose )
{
    word pTruth[4] = {0xcafe, 0, 0, 0};
    Abc_Ntk_t * pNtk;
    Gia_Man_t * pGia, * pGia2, * pGia3, * pGia4, * pMiter;
    Cec_ParCec_t ParsCec, * pPars = &ParsCec;
    int pArrivalTimes[4] = {6, 2, 8, 5};

    Cec_ManCecSetDefaultParams( pPars );

    pNtk = Abc_NtkFromTruthTable( pTruth, 4 );
    Abc_NtkToAig( pNtk );
    pGia = Abc_NtkAigToGia( pNtk, 1 );

    pGia2 = Gia_ManFindExact( pTruth, 4, 1, -1, NULL, fVerbose );
    pMiter = Gia_ManMiter( pGia, pGia2, 0, 1, 0, 0, 1 );
    assert( pMiter );
    Cec_ManVerify( pMiter, pPars );
    Gia_ManStop( pMiter );

    pGia3 = Gia_ManFindExact( pTruth, 4, 1, 3, NULL, fVerbose );
    pMiter = Gia_ManMiter( pGia, pGia3, 0, 1, 0, 0, 1 );
    assert( pMiter );
    Cec_ManVerify( pMiter, pPars );
    Gia_ManStop( pMiter );

    pGia4 = Gia_ManFindExact( pTruth, 4, 1, 9, pArrivalTimes, fVerbose );
    pMiter = Gia_ManMiter( pGia, pGia4, 0, 1, 0, 0, 1 );
    assert( pMiter );
    Cec_ManVerify( pMiter, pPars );
    Gia_ManStop( pMiter );

    assert( !Gia_ManFindExact( pTruth, 4, 1, 2, NULL, fVerbose ) );

    assert( !Gia_ManFindExact( pTruth, 4, 1, 8, pArrivalTimes, fVerbose ) );

    Gia_ManStop( pGia );
    Gia_ManStop( pGia2 );
    Gia_ManStop( pGia3 );
    Gia_ManStop( pGia4 );
}

void Abc_ExactTest( int fVerbose )
{
    Abc_ExactTestSingleOutput( fVerbose );
    Abc_ExactTestSingleOutputAIG( fVerbose );

    printf( "\n" );
}


////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END
