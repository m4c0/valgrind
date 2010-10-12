
//--------------------------------------------------------------------*/
//--- DHAT: a Dynamic Heap Analysis Tool                 dh_main.c ---*/
//--------------------------------------------------------------------*/

/*
   This file is part of DHAT, a Valgrind tool for profiling the
   heap usage of programs.

   Copyright (C) 2010-2010 Mozilla Inc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

/* Contributed by Julian Seward <jseward@acm.org> */


#include "pub_tool_basics.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_machine.h"      // VG_(fnptr_to_fnentry)
#include "pub_tool_mallocfree.h"
#include "pub_tool_replacemalloc.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_wordfm.h"

#define HISTOGRAM_SIZE_LIMIT 4096 //1024


//------------------------------------------------------------//
//--- Globals                                              ---//
//------------------------------------------------------------//

// Number of guest instructions executed so far.  This is 
// incremented directly from the generated code.
static ULong g_guest_instrs_executed = 0;

// Summary statistics for the entire run.
static ULong g_tot_blocks = 0;   // total blocks allocated
static ULong g_tot_bytes  = 0;   // total bytes allocated

static ULong g_cur_blocks_live = 0; // curr # blocks live
static ULong g_cur_bytes_live  = 0; // curr # bytes live

static ULong g_max_blocks_live = 0; // bytes and blocks at
static ULong g_max_bytes_live  = 0; // the max residency point


//------------------------------------------------------------//
//--- an Interval Tree of live blocks                      ---//
//------------------------------------------------------------//

/* Tracks information about live blocks. */
typedef
   struct {
      Addr        payload;
      SizeT       req_szB;
      ExeContext* ap;  /* allocation ec */
      ULong       allocd_at; /* instruction number */
      ULong       n_reads;
      ULong       n_writes;
      /* Approx histogram, one byte per payload byte.  Counts latch up
         therefore at 255.  Can be NULL if the block is resized or if
         the block is larger than HISTOGRAM_SIZE_LIMIT. */
      UChar*      histoB; /* [0 .. req_szB-1] */
   }
   Block;

/* May not contain zero-sized blocks.  May not contain
   overlapping blocks. */
static WordFM* interval_tree = NULL;  /* WordFM* Block* void */

/* Here's the comparison function.  Since the tree is required
to contain non-zero sized, non-overlapping blocks, it's good
enough to consider any overlap as a match. */
static Word interval_tree_Cmp ( UWord k1, UWord k2 )
{
   Block* b1 = (Block*)k1;
   Block* b2 = (Block*)k2;
   tl_assert(b1->req_szB > 0);
   tl_assert(b2->req_szB > 0);
   if (b1->payload + b1->req_szB <= b2->payload) return -1;
   if (b2->payload + b2->req_szB <= b1->payload) return  1;
   return 0;
}

static Block* find_Block_containing ( Addr a )
{
   Block fake;
   fake.payload = a;
   fake.req_szB = 1;
   UWord foundkey = 1;
   UWord foundval = 1;
   Bool found = VG_(lookupFM)( interval_tree,
                               &foundkey, &foundval, (UWord)&fake );
   if (!found)
      return NULL;
   tl_assert(foundval == 0); // we don't store vals in the interval tree
   tl_assert(foundkey != 1);
   return (Block*)foundkey;
}

// delete a block; asserts if not found.  (viz, 'a' must be
// known to be present.)
static void delete_Block_starting_at ( Addr a )
{
   Block fake;
   fake.payload = a;
   fake.req_szB = 1;
   Bool found = VG_(delFromFM)( interval_tree,
                                NULL, NULL, (Addr)&fake );
   tl_assert(found);
}


//------------------------------------------------------------//
//--- a FM of allocation points (APs)                      ---//
//------------------------------------------------------------//

typedef
   struct {
      // the allocation point that we're summarising stats for
      ExeContext* ap;
      // used when printing results
      Bool shown;
      // The current number of blocks and bytes live for this AP
      ULong cur_blocks_live;
      ULong cur_bytes_live;
      // The number of blocks and bytes live at the max-liveness
      // point.  Note this is a bit subtle.  max_blocks_live is not
      // the maximum number of live blocks, but rather the number of
      // blocks live at the point of maximum byte liveness.  These are
      // not necessarily the same thing.
      ULong max_blocks_live;
      ULong max_bytes_live;
      // Total number of blocks and bytes allocated by this AP.
      ULong tot_blocks;
      ULong tot_bytes;
      // Sum of death ages for all blocks allocated by this AP,
      // that have subsequently been freed.
      ULong death_ages_sum;
      ULong deaths;
      // Total number of reads and writes in all blocks allocated
      // by this AP.
      ULong n_reads;
      ULong n_writes;
      /* Histogram information.  We maintain a histogram aggregated for
         all retiring Blocks allocated by this AP, but only if:
         - this AP has only ever allocated objects of one size
         - that size is <= HISTOGRAM_SIZE_LIMIT
         What we need therefore is a mechanism to see if this AP
         has only ever allocated blocks of one size.

         3 states:
            Unknown          because no retirement yet 
            Exactly xsize    all retiring blocks are of this size
            Mixed            multiple different sizes seen
      */
      enum { Unknown=999, Exactly, Mixed } xsize_tag;
      SizeT xsize;
      UInt* histo; /* [0 .. xsize-1] */
   }
   APInfo;

/* maps ExeContext*'s to APInfo*'s.  Note that the keys must match the
   .ap field in the values. */
static WordFM* apinfo = NULL;  /* WordFM* ExeContext* APInfo* */


/* 'bk' is being introduced (has just been allocated).  Find the
   relevant APInfo entry for it, or create one, based on the block's
   allocation EC.  Then, update the APInfo to the extent that we
   actually can, to reflect the allocation. */
static void intro_Block ( Block* bk )
{
   tl_assert(bk);
   tl_assert(bk->ap);

   APInfo* api   = NULL;
   UWord   keyW  = 0;
   UWord   valW  = 0;
   Bool    found = VG_(lookupFM)( apinfo,
                                  &keyW, &valW, (UWord)bk->ap );
   if (found) {
      api = (APInfo*)valW;
      tl_assert(keyW == (UWord)bk->ap);
   } else {
      api = VG_(malloc)( "dh.main.intro_Block.1", sizeof(APInfo) );
      VG_(memset)(api, 0, sizeof(*api));
      api->ap = bk->ap;
      Bool present = VG_(addToFM)( apinfo,
                                   (UWord)bk->ap, (UWord)api );
      tl_assert(!present);
      // histo stuff
      tl_assert(api->deaths == 0);
      api->xsize_tag = Unknown;
      api->xsize = 0;
      VG_(printf)("api %p   -->  Unknown\n", api);
   }

   tl_assert(api->ap == bk->ap);

   /* So: update stats to reflect an allocation */

   // # live blocks
   api->cur_blocks_live++;

   // # live bytes
   api->cur_bytes_live += bk->req_szB;
   if (api->cur_bytes_live > api->max_bytes_live) {
      api->max_bytes_live  = api->cur_bytes_live;
      api->max_blocks_live = api->cur_blocks_live;
   }

   // total blocks and bytes allocated here
   api->tot_blocks++;
   api->tot_bytes += bk->req_szB;

   // update summary globals
   g_tot_blocks++;
   g_tot_bytes += bk->req_szB;

   g_cur_blocks_live++;
   g_cur_bytes_live += bk->req_szB;
   if (g_cur_bytes_live > g_max_bytes_live) {
      g_max_bytes_live = g_cur_bytes_live;
      g_max_blocks_live = g_cur_blocks_live;
   }
}


/* 'bk' is retiring (being freed).  Find the relevant APInfo entry for
   it, which must already exist.  Then, fold info from 'bk' into that
   entry. */
static void retire_Block ( Block* bk )
{
   tl_assert(bk);
   tl_assert(bk->ap);

   APInfo* api   = NULL;
   UWord   keyW  = 0;
   UWord   valW  = 0;
   Bool    found = VG_(lookupFM)( apinfo,
                                  &keyW, &valW, (UWord)bk->ap );

   tl_assert(found);
   api = (APInfo*)valW;
   tl_assert(api->ap == bk->ap);

   // update stats following this free.
   if (0)
   VG_(printf)("ec %p  api->c_by_l %llu  bk->rszB %llu\n",
               bk->ap, api->cur_bytes_live, (ULong)bk->req_szB);

   tl_assert(api->cur_blocks_live >= 1);
   tl_assert(api->cur_bytes_live >= bk->req_szB);
   api->cur_blocks_live--;
   api->cur_bytes_live -= bk->req_szB;

   api->deaths++;

   tl_assert(bk->allocd_at <= g_guest_instrs_executed);
   api->death_ages_sum += (g_guest_instrs_executed - bk->allocd_at);

   api->n_reads  += bk->n_reads;
   api->n_writes += bk->n_writes;

   // update global summary stats
   tl_assert(g_cur_blocks_live > 0);
   g_cur_blocks_live--;
   tl_assert(g_cur_bytes_live >= bk->req_szB);
   g_cur_bytes_live -= bk->req_szB;

   // histo stuff.  First, do state transitions for xsize/xsize_tag.
   switch (api->xsize_tag) {

      case Unknown:
         tl_assert(api->xsize == 0);
         tl_assert(api->deaths == 1);
         tl_assert(!api->histo);
         api->xsize_tag = Exactly;
         api->xsize = bk->req_szB;
         VG_(printf)("api %p   -->  Exactly(%lu)\n", api, api->xsize);
         // and allocate the histo
         if (bk->histoB) {
            api->histo = VG_(malloc)("dh.main.retire_Block.1", api->xsize * sizeof(UInt));
            VG_(memset)(api->histo, 0, api->xsize * sizeof(UInt));
         }
         break;

      case Exactly:
         tl_assert(api->deaths > 1);
         if (bk->req_szB != api->xsize) {
            VG_(printf)("api %p   -->  Mixed(%lu -> %lu)\n",
                        api, api->xsize, bk->req_szB);
            api->xsize_tag = Mixed;
            api->xsize = 0;
            // deallocate the histo, if any
            if (api->histo) {
               VG_(free)(api->histo);
               api->histo = NULL;
            }
         }
         break;

      case Mixed:
         tl_assert(api->deaths > 1);
         break;

      default:
        tl_assert(0);
   }

   // See if we can fold the histo data from this block into
   // the data for the AP
   if (api->xsize_tag == Exactly && api->histo && bk->histoB) {
      tl_assert(api->xsize == bk->req_szB);
      UWord i;
      for (i = 0; i < api->xsize; i++) {
         // FIXME: do something sane in case of overflow of api->histo[..]
         api->histo[i] += (UInt)bk->histoB[i];
      }
      VG_(printf)("fold in, AP = %p\n", api);
   }



#if 0
   if (bk->histoB) {
      VG_(printf)("block retiring, histo %lu: ", bk->req_szB);
      UWord i;
      for (i = 0; i < bk->req_szB; i++)
        VG_(printf)("%u ", (UInt)bk->histoB[i]);
      VG_(printf)("\n");
   } else {
      VG_(printf)("block retiring, no histo %lu\n", bk->req_szB);
   }
#endif
}

/* This handles block resizing.  When a block with AP 'ec' has a
   size change of 'delta', call here to update the APInfo. */
static void apinfo_change_cur_bytes_live( ExeContext* ec, Long delta )
{
   APInfo* api   = NULL;
   UWord   keyW  = 0;
   UWord   valW  = 0;
   Bool    found = VG_(lookupFM)( apinfo,
                                  &keyW, &valW, (UWord)ec );

   tl_assert(found);
   api = (APInfo*)valW;
   tl_assert(api->ap == ec);

   if (delta < 0) {
      tl_assert(api->cur_bytes_live >= -delta);
      tl_assert(g_cur_bytes_live >= -delta);
   }

   // adjust current live size
   api->cur_bytes_live += delta;
   g_cur_bytes_live += delta;

   if (delta > 0 && api->cur_bytes_live > api->max_bytes_live) {
      api->max_bytes_live  = api->cur_bytes_live;
      api->max_blocks_live = api->cur_blocks_live;
   }

   // update global summary stats
   if (delta > 0 && g_cur_bytes_live > g_max_bytes_live) {
      g_max_bytes_live = g_cur_bytes_live;
      g_max_blocks_live = g_cur_blocks_live;
   }

   // adjust total allocation size
   if (delta > 0)
      api->tot_bytes += delta;
}


//------------------------------------------------------------//
//--- update both Block and APInfos after {m,re}alloc/free ---//
//------------------------------------------------------------//

static
void* new_block ( ThreadId tid, void* p, SizeT req_szB, SizeT req_alignB,
                  Bool is_zeroed )
{
   tl_assert(p == NULL); // don't handle custom allocators right now
   SizeT actual_szB, slop_szB;

   if ((SSizeT)req_szB < 0) return NULL;

   if (req_szB == 0)
      req_szB = 1;  /* can't allow zero-sized blocks in the interval tree */

   // Allocate and zero if necessary
   if (!p) {
      p = VG_(cli_malloc)( req_alignB, req_szB );
      if (!p) {
         return NULL;
      }
      if (is_zeroed) VG_(memset)(p, 0, req_szB);
      actual_szB = VG_(malloc_usable_size)(p);
      tl_assert(actual_szB >= req_szB);
      slop_szB = actual_szB - req_szB;
   } else {
      slop_szB = 0;
   }

   // Make new HP_Chunk node, add to malloc_list
   Block* bk = VG_(malloc)("dh.new_block.1", sizeof(Block));
   bk->payload   = (Addr)p;
   bk->req_szB   = req_szB;
   bk->ap        = VG_(record_ExeContext)(tid, 0/*first word delta*/);
   bk->allocd_at = g_guest_instrs_executed;
   bk->n_reads   = 0;
   bk->n_writes  = 0;
   // set up histogram array, if the block isn't too large
   bk->histoB = NULL;
   if (req_szB <= HISTOGRAM_SIZE_LIMIT) {
      bk->histoB = VG_(malloc)("dh.new_block.2", req_szB);
      VG_(memset)(bk->histoB, 0, req_szB);
   }

   Bool present = VG_(addToFM)( interval_tree, (UWord)bk, (UWord)0/*no val*/);
   tl_assert(!present);

   intro_Block(bk);

   if (0) VG_(printf)("ALLOC %ld -> %p\n", req_szB, p);

   return p;
}

static
void die_block ( void* p, Bool custom_free )
{
   tl_assert(!custom_free);  // at least for now

   Block* bk = find_Block_containing( (Addr)p );

   if (!bk) {
     return; // bogus free
   }

   tl_assert(bk->req_szB > 0);
   // assert the block finder is behaving sanely
   tl_assert(bk->payload <= (Addr)p);
   tl_assert( (Addr)p < bk->payload + bk->req_szB );

   if (bk->payload != (Addr)p) {
      return; // bogus free
   }

   if (0) VG_(printf)(" FREE %p %llu\n",
                      p, g_guest_instrs_executed - bk->allocd_at);

   retire_Block(bk);

   VG_(cli_free)( (void*)bk->payload );
   delete_Block_starting_at( bk->payload );
   if (bk->histoB) {
      VG_(free)( bk->histoB );
      bk->histoB = NULL;
   }
   VG_(free)( bk );
}


static
void* renew_block ( ThreadId tid, void* p_old, SizeT new_req_szB )
{
   if (0) VG_(printf)("REALL %p %ld\n", p_old, new_req_szB);
   void* p_new = NULL;

   tl_assert(new_req_szB > 0); // map 0 to 1

   // Find the old block.
   Block* bk = find_Block_containing( (Addr)p_old );
   if (!bk) {
      return NULL;   // bogus realloc
   }

   tl_assert(bk->req_szB > 0);
   // assert the block finder is behaving sanely
   tl_assert(bk->payload <= (Addr)p_old);
   tl_assert( (Addr)p_old < bk->payload + bk->req_szB );

   if (bk->payload != (Addr)p_old) {
      return NULL; // bogus realloc
   }

// Keeping the histogram alive in any meaningful way across
// block resizing is too darn complicated.  Just throw it away.
if (bk->histoB) {
  VG_(free)(bk->histoB);
  bk->histoB = NULL;
}

   // Actually do the allocation, if necessary.
   if (new_req_szB <= bk->req_szB) {

      // New size is smaller or same; block not moved.
      apinfo_change_cur_bytes_live(bk->ap,
                                   (Long)new_req_szB - (Long)bk->req_szB);
      bk->req_szB = new_req_szB;
      return p_old;

   } else {

      // New size is bigger;  make new block, copy shared contents, free old.
      p_new = VG_(cli_malloc)(VG_(clo_alignment), new_req_szB);
      if (!p_new) {
         // Nb: if realloc fails, NULL is returned but the old block is not
         // touched.  What an awful function.
         return NULL;
      }
      tl_assert(p_new != p_old);

      VG_(memcpy)(p_new, p_old, bk->req_szB);
      VG_(cli_free)(p_old);

      // Since the block has moved, we need to re-insert it into the
      // interval tree at the new place.  Do this by removing
      // and re-adding it.
      delete_Block_starting_at( (Addr)p_old );
      // now 'bk' is no longer in the tree, but the Block itself
      // is still alive

      // Update the metadata.
      apinfo_change_cur_bytes_live(bk->ap,
                                   (Long)new_req_szB - (Long)bk->req_szB);
      bk->payload = (Addr)p_new;
      bk->req_szB = new_req_szB;

      // and re-add
      Bool present
         = VG_(addToFM)( interval_tree, (UWord)bk, (UWord)0/*no val*/); 
      tl_assert(!present);

      return p_new;
   }
   /*NOTREACHED*/
   tl_assert(0);
}


//------------------------------------------------------------//
//--- malloc() et al replacement wrappers                  ---//
//------------------------------------------------------------//

static void* dh_malloc ( ThreadId tid, SizeT szB )
{
   return new_block( tid, NULL, szB, VG_(clo_alignment), /*is_zeroed*/False );
}

static void* dh___builtin_new ( ThreadId tid, SizeT szB )
{
   return new_block( tid, NULL, szB, VG_(clo_alignment), /*is_zeroed*/False );
}

static void* dh___builtin_vec_new ( ThreadId tid, SizeT szB )
{
   return new_block( tid, NULL, szB, VG_(clo_alignment), /*is_zeroed*/False );
}

static void* dh_calloc ( ThreadId tid, SizeT m, SizeT szB )
{
   return new_block( tid, NULL, m*szB, VG_(clo_alignment), /*is_zeroed*/True );
}

static void *dh_memalign ( ThreadId tid, SizeT alignB, SizeT szB )
{
   return new_block( tid, NULL, szB, alignB, False );
}

static void dh_free ( ThreadId tid __attribute__((unused)), void* p )
{
   die_block( p, /*custom_free*/False );
}

static void dh___builtin_delete ( ThreadId tid, void* p )
{
   die_block( p, /*custom_free*/False);
}

static void dh___builtin_vec_delete ( ThreadId tid, void* p )
{
   die_block( p, /*custom_free*/False );
}

static void* dh_realloc ( ThreadId tid, void* p_old, SizeT new_szB )
{
   if (p_old == NULL) {
      return dh_malloc(tid, new_szB);
   }
   if (new_szB == 0) {
      dh_free(tid, p_old);
      return NULL;
   }
   return renew_block(tid, p_old, new_szB);
}

static SizeT dh_malloc_usable_size ( ThreadId tid, void* p )
{                                                            
   tl_assert(0);
//zz   HP_Chunk* hc = VG_(HT_lookup)( malloc_list, (UWord)p );
//zz
//zz   return ( hc ? hc->req_szB + hc->slop_szB : 0 );
}                                                            

//------------------------------------------------------------//
//--- memory references                                    ---//
//------------------------------------------------------------//

static
void inc_histo_for_block ( Block* bk, Addr addr, UWord szB )
{
   UWord i, offMin, offMax1;
   offMin = addr - bk->payload;
   tl_assert(offMin < bk->req_szB);
   offMax1 = offMin + szB;
   if (offMax1 > bk->req_szB)
      offMax1 = bk->req_szB;
   //VG_(printf)("%lu %lu   (size of block %lu)\n", offMin, offMax1, bk->req_szB);
   for (i = offMin; i < offMax1; i++) {
      UChar n = bk->histoB[i];
      if (n < 255) n++;
      bk->histoB[i] = n;
   }
}

static VG_REGPARM(2)
void dh_handle_write ( Addr addr, UWord szB )
{
   Block* bk = find_Block_containing(addr);
   if (bk) {
      bk->n_writes += szB;
      if (bk->histoB)
         inc_histo_for_block(bk, addr, szB);
   }
}

static VG_REGPARM(2)
void dh_handle_read ( Addr addr, UWord szB )
{
   Block* bk = find_Block_containing(addr);
   if (bk) {
      bk->n_reads += szB;
      if (bk->histoB)
         inc_histo_for_block(bk, addr, szB);
   }
}


// Handle reads and writes by syscalls (read == kernel
// reads user space, write == kernel writes user space).
// Assumes no such read or write spans a heap block
// boundary and so we can treat it just as one giant
// read or write.
static
void dh_handle_noninsn_read ( CorePart part, ThreadId tid, Char* s,
                              Addr base, SizeT size )
{
   switch (part) {
      case Vg_CoreSysCall:
         dh_handle_read(base, size);
         break;
      case Vg_CoreSysCallArgInMem:
         break;
      case Vg_CoreTranslate:
         break;
      default:
         tl_assert(0);
   }
}

static
void dh_handle_noninsn_write ( CorePart part, ThreadId tid,
                               Addr base, SizeT size )
{
   switch (part) {
      case Vg_CoreSysCall:
         dh_handle_write(base, size);
         break;
      case Vg_CoreSignal:
         break;
      default:
         tl_assert(0);
   }
}


//------------------------------------------------------------//
//--- Instrumentation                                      ---//
//------------------------------------------------------------//

static
void add_counter_update(IRSB* sbOut, Int n)
{
   #if defined(VG_BIGENDIAN)
   # define END Iend_BE
   #elif defined(VG_LITTLEENDIAN)
   # define END Iend_LE
   #else
   # error "Unknown endianness"
   #endif
   // Add code to increment 'g_guest_instrs_executed' by 'n', like this:
   //   WrTmp(t1, Load64(&g_guest_instrs_executed))
   //   WrTmp(t2, Add64(RdTmp(t1), Const(n)))
   //   Store(&g_guest_instrs_executed, t2)
   IRTemp t1 = newIRTemp(sbOut->tyenv, Ity_I64);
   IRTemp t2 = newIRTemp(sbOut->tyenv, Ity_I64);
   IRExpr* counter_addr = mkIRExpr_HWord( (HWord)&g_guest_instrs_executed );

   IRStmt* st1 = IRStmt_WrTmp(t1, IRExpr_Load(END, Ity_I64, counter_addr));
   IRStmt* st2 =
      IRStmt_WrTmp(t2,
                   IRExpr_Binop(Iop_Add64, IRExpr_RdTmp(t1),
                                           IRExpr_Const(IRConst_U64(n))));
   IRStmt* st3 = IRStmt_Store(END, counter_addr, IRExpr_RdTmp(t2));

   addStmtToIRSB( sbOut, st1 );
   addStmtToIRSB( sbOut, st2 );
   addStmtToIRSB( sbOut, st3 );
}

static
void addMemEvent(IRSB* sbOut, Bool isWrite, Int szB, IRExpr* addr )
{
   IRType   tyAddr   = Ity_INVALID;
   HChar*   hName    = NULL;
   void*    hAddr    = NULL;
   IRExpr** argv     = NULL;
   IRDirty* di       = NULL;

   tyAddr = typeOfIRExpr( sbOut->tyenv, addr );
   tl_assert(tyAddr == Ity_I32 || tyAddr == Ity_I64);

   if (isWrite) {
      hName = "dh_handle_write";
      hAddr = &dh_handle_write;
   } else {
      hName = "dh_handle_read";
      hAddr = &dh_handle_read;
   }

   argv = mkIRExprVec_2( addr, mkIRExpr_HWord(szB) );

   /* Add the helper. */
   tl_assert(hName);
   tl_assert(hAddr);
   tl_assert(argv);
   di = unsafeIRDirty_0_N( 2/*regparms*/,
                           hName, VG_(fnptr_to_fnentry)( hAddr ),
                           argv );

   addStmtToIRSB( sbOut, IRStmt_Dirty(di) );
}

static
IRSB* dh_instrument ( VgCallbackClosure* closure,
                      IRSB* sbIn,
                      VexGuestLayout* layout,
                      VexGuestExtents* vge,
                      IRType gWordTy, IRType hWordTy )
{
   Int   i, n = 0;
   IRSB* sbOut;
   IRTypeEnv* tyenv = sbIn->tyenv;

   // We increment the instruction count in two places:
   // - just before any Ist_Exit statements;
   // - just before the IRSB's end.
   // In the former case, we zero 'n' and then continue instrumenting.
   
   sbOut = deepCopyIRSBExceptStmts(sbIn);

   // Copy verbatim any IR preamble preceding the first IMark
   i = 0;
   while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
      addStmtToIRSB( sbOut, sbIn->stmts[i] );
      i++;
   }
   
   for (/*use current i*/; i < sbIn->stmts_used; i++) {
      IRStmt* st = sbIn->stmts[i];
      
      if (!st || st->tag == Ist_NoOp) continue;

      switch (st->tag) {

         case Ist_IMark: {
            n++;
            break;
         }

         case Ist_Exit: {
            if (n > 0) {
               // Add an increment before the Exit statement, then reset 'n'.
               add_counter_update(sbOut, n);
               n = 0;
            }
            break;
         }

         case Ist_WrTmp: {
            IRExpr* data = st->Ist.WrTmp.data;
            if (data->tag == Iex_Load) {
               IRExpr* aexpr = data->Iex.Load.addr;
               // Note also, endianness info is ignored.  I guess
               // that's not interesting.
               addMemEvent( sbOut, False/*!isWrite*/,
                            sizeofIRType(data->Iex.Load.ty), aexpr );
            }
            break;
         }

         case Ist_Store: {
            IRExpr* data  = st->Ist.Store.data;
            IRExpr* aexpr = st->Ist.Store.addr;
            addMemEvent( sbOut, True/*isWrite*/, 
                         sizeofIRType(typeOfIRExpr(tyenv, data)), aexpr );
            break;
         }

         case Ist_Dirty: {
            Int      dataSize;
            IRDirty* d = st->Ist.Dirty.details;
            if (d->mFx != Ifx_None) {
               /* This dirty helper accesses memory.  Collect the details. */
               tl_assert(d->mAddr != NULL);
               tl_assert(d->mSize != 0);
               dataSize = d->mSize;
               // Large (eg. 28B, 108B, 512B on x86) data-sized
               // instructions will be done inaccurately, but they're
               // very rare and this avoids errors from hitting more
               // than two cache lines in the simulation.
               if (d->mFx == Ifx_Read || d->mFx == Ifx_Modify)
                  addMemEvent( sbOut, False/*!isWrite*/,
                               dataSize, d->mAddr );
               if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify)
                  addMemEvent( sbOut, True/*isWrite*/,
                               dataSize, d->mAddr );
            } else {
               tl_assert(d->mAddr == NULL);
               tl_assert(d->mSize == 0);
            }
            break;
         }

         case Ist_CAS: {
            /* We treat it as a read and a write of the location.  I
               think that is the same behaviour as it was before IRCAS
               was introduced, since prior to that point, the Vex
               front ends would translate a lock-prefixed instruction
               into a (normal) read followed by a (normal) write. */
            Int    dataSize;
            IRCAS* cas = st->Ist.CAS.details;
            tl_assert(cas->addr != NULL);
            tl_assert(cas->dataLo != NULL);
            dataSize = sizeofIRType(typeOfIRExpr(tyenv, cas->dataLo));
            if (cas->dataHi != NULL)
               dataSize *= 2; /* since it's a doubleword-CAS */
            addMemEvent( sbOut, False/*!isWrite*/, dataSize, cas->addr );
            addMemEvent( sbOut, True/*isWrite*/,   dataSize, cas->addr );
            break;
         }

         case Ist_LLSC: {
            IRType dataTy;
            if (st->Ist.LLSC.storedata == NULL) {
               /* LL */
               dataTy = typeOfIRTemp(tyenv, st->Ist.LLSC.result);
               addMemEvent( sbOut, False/*!isWrite*/,
                            sizeofIRType(dataTy), st->Ist.LLSC.addr );
            } else {
               /* SC */
               dataTy = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
               addMemEvent( sbOut, True/*isWrite*/,
                            sizeofIRType(dataTy), st->Ist.LLSC.addr );
            }
            break;
         }

         default:
            break;
      }

      addStmtToIRSB( sbOut, st );
   }

   if (n > 0) {
      // Add an increment before the SB end.
      add_counter_update(sbOut, n);
   }
   return sbOut;
}


//------------------------------------------------------------//
//--- Finalisation                                         ---//
//------------------------------------------------------------//

static void show_N_div_100( /*OUT*/HChar* buf, ULong n )
{
   ULong nK = n / 100;
   ULong nR = n % 100;
   VG_(sprintf)(buf, "%llu.%s%llu", nK,
                nR < 10 ? "0" : "",
                nR);
}

static void show_APInfo ( APInfo* api )
{
   VG_(umsg)("max_live:    %'llu in %'llu blocks\n",
             api->max_bytes_live, api->max_blocks_live);
   VG_(umsg)("tot_alloc:   %'llu in %'llu blocks\n",
             api->tot_bytes, api->tot_blocks);

   tl_assert(api->tot_blocks >= api->max_blocks_live);
   tl_assert(api->tot_bytes >= api->max_bytes_live);

   if (api->deaths > 0) {
      VG_(umsg)("deaths:      %'llu, at avg age %'llu\n",
                api->deaths,
                api->deaths == 0
                   ? 0 : (api->death_ages_sum / api->deaths));
   } else {
      VG_(umsg)("deaths:      none (none of these blocks were freed)\n");
   }

   HChar bufR[80], bufW[80];
   VG_(memset)(bufR, 0, sizeof(bufR));
   VG_(memset)(bufW, 0, sizeof(bufW));
   if (api->tot_bytes > 0) {
      show_N_div_100(bufR, (100ULL * api->n_reads) / api->tot_bytes);
      show_N_div_100(bufW, (100ULL * api->n_writes) / api->tot_bytes);
   } else {
      VG_(strcat)(bufR, "Inf");
      VG_(strcat)(bufW, "Inf");
   }

   VG_(umsg)("acc-ratios:  %s rd, %s wr "
             " (%'llu b-read, %'llu b-written)\n",
             bufR, bufW,
             api->n_reads, api->n_writes);

   VG_(pp_ExeContext)(api->ap);

   if (api->histo && api->xsize_tag == Exactly) {
      VG_(umsg)("\nAggregated access counts by offset:\n");
      VG_(umsg)("\n");
      UWord i;
      if (api->xsize > 0)
         VG_(umsg)("[   0]  ");
      for (i = 0; i < api->xsize; i++) {
         if (i > 0 && (i % 16) == 0 && i != api->xsize-1) {
            VG_(umsg)("\n");
            VG_(umsg)("[%4lu]  ", i);
         }
         VG_(umsg)("%u ", api->histo[i]);
      }
      VG_(umsg)("\n\n");
   }
}


static ULong get_metric__max_bytes_live ( APInfo* api ) {
   return api->max_bytes_live;
}
static ULong get_metric__tot_bytes ( APInfo* api ) {
   return api->tot_bytes;
}
static ULong get_metric__max_blocks_live ( APInfo* api ) {
   return api->max_blocks_live;
}

static void show_topN_apinfos ( ULong(*get_metric)(APInfo*),
                                HChar* metric_name,
                                Bool   increasing )
{
   Int i;
   const Int N = 50000; //200 -150;

   UWord keyW, valW;

   VG_(umsg)("\n");
   VG_(umsg)("======== ORDERED BY %s \"%s\": "
             "top %d allocators ========\n", 
             increasing ? "increasing" : "decreasing",
             metric_name, N );

   // Clear all .shown bits
   VG_(initIterFM)( apinfo );
   while (VG_(nextIterFM)( apinfo, &keyW, &valW )) {
      APInfo* api = (APInfo*)valW;
      tl_assert(api && api->ap == (ExeContext*)keyW);
      api->shown = False;
   }
   VG_(doneIterFM)( apinfo );

   // Now print the top N entries.  Each one requires a 
   // complete scan of the set.  Duh.
   for (i = 0; i < N; i++) {
      ULong   best_metric = increasing ? ~0ULL : 0ULL;
      APInfo* best_api    = NULL;

      VG_(initIterFM)( apinfo );
      while (VG_(nextIterFM)( apinfo, &keyW, &valW )) {
         APInfo* api = (APInfo*)valW;
         if (api->shown)
            continue;
         ULong metric = get_metric(api);
         if (increasing ? (metric < best_metric) : (metric > best_metric)) {
            best_metric = metric;
            best_api = api;
         }
      }
      VG_(doneIterFM)( apinfo );

      if (!best_api)
         break; // all APIs have been shown.  Stop.

      VG_(umsg)("\n");
      VG_(umsg)("------ %d of %d ------\n", i+1, N );
      show_APInfo(best_api);
      best_api->shown = True;
   }

   VG_(umsg)("\n");
}


static void dh_fini(Int exit_status)
{
   VG_(umsg)("======== SUMMARY STATISTICS ========\n");
   VG_(umsg)("\n");
   VG_(umsg)("guest_insns:  %'llu\n", g_guest_instrs_executed);
   VG_(umsg)("\n");
   VG_(umsg)("max_live:     %'llu in %'llu blocks\n",
             g_max_bytes_live, g_max_blocks_live);
   VG_(umsg)("\n");
   VG_(umsg)("tot_alloc:    %'llu in %'llu blocks\n",
             g_tot_bytes, g_tot_blocks);
   VG_(umsg)("\n");
   if (g_tot_bytes > 0) {
      VG_(umsg)("insns per allocated byte: %'llu\n",
                g_guest_instrs_executed / g_tot_bytes);
      VG_(umsg)("\n");
   }

   if (0)
   show_topN_apinfos( get_metric__max_bytes_live,
                      "max_bytes_live", False/*highest first*/ );
   if (1)
   show_topN_apinfos( get_metric__max_blocks_live,
                      "max_blocks_live", False/*highest first*/ );

   if (0)
   show_topN_apinfos( get_metric__tot_bytes,
                      "tot_bytes", False/*highest first*/ );
}


//------------------------------------------------------------//
//--- Initialisation                                       ---//
//------------------------------------------------------------//

static void dh_post_clo_init(void)
{
}

static void dh_pre_clo_init(void)
{
   VG_(details_name)            ("DHAT");
   VG_(details_version)         (NULL);
   VG_(details_description)     ("a dynamic heap analysis tool");
   VG_(details_copyright_author)(
      "Copyright (C) 2010-2010, and GNU GPL'd, by Mozilla Inc");
   VG_(details_bug_reports_to)  (VG_BUGS_TO);

   // Basic functions.
   VG_(basic_tool_funcs)          (dh_post_clo_init,
                                   dh_instrument,
                                   dh_fini);
//zz
//zz   // Needs.
//zz   VG_(needs_libc_freeres)();
//zz   VG_(needs_command_line_options)(dh_process_cmd_line_option,
//zz                                   dh_print_usage,
//zz                                   dh_print_debug_usage);
//zz   VG_(needs_client_requests)     (dh_handle_client_request);
//zz   VG_(needs_sanity_checks)       (dh_cheap_sanity_check,
//zz                                   dh_expensive_sanity_check);
   VG_(needs_malloc_replacement)  (dh_malloc,
                                   dh___builtin_new,
                                   dh___builtin_vec_new,
                                   dh_memalign,
                                   dh_calloc,
                                   dh_free,
                                   dh___builtin_delete,
                                   dh___builtin_vec_delete,
                                   dh_realloc,
                                   dh_malloc_usable_size,
                                   0 );

   VG_(track_pre_mem_read)        ( dh_handle_noninsn_read );
   //VG_(track_pre_mem_read_asciiz) ( check_mem_is_defined_asciiz );
   VG_(track_post_mem_write)      ( dh_handle_noninsn_write );

//zz   // HP_Chunks.
//zz   malloc_list = VG_(HT_construct)( "Massif's malloc list" );
//zz
//zz   // Dummy node at top of the context structure.
//zz   alloc_xpt = new_XPt(/*ip*/0, /*parent*/NULL);
//zz
//zz   // Initialise alloc_fns and ignore_fns.
//zz   init_alloc_fns();
//zz   init_ignore_fns();
//zz
//zz   // Initialise args_for_massif.
//zz   args_for_massif = VG_(newXA)(VG_(malloc), "ms.main.mprci.1", 
//zz                                VG_(free), sizeof(HChar*));
   tl_assert(!interval_tree);

   interval_tree = VG_(newFM)( VG_(malloc),
                               "dh.main.interval_tree.1",
                               VG_(free),
                               interval_tree_Cmp );

   apinfo = VG_(newFM)( VG_(malloc),
                        "dh.main.apinfo.1",
                        VG_(free),
                        NULL/*unboxedcmp*/ );
}

VG_DETERMINE_INTERFACE_VERSION(dh_pre_clo_init)

//--------------------------------------------------------------------//
//--- end                                                dh_main.c ---//
//--------------------------------------------------------------------//
