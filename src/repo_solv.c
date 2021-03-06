/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_solv.c
 * 
 * Add a repo in solv format
 * 
 */



#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "repo_solv.h"
#include "util.h"

#include "repopack.h"
#include "repopage.h"

#include "poolid_private.h"	/* WHATPROVIDES_BLOCK */

#define INTERESTED_START	SOLVABLE_NAME
#define INTERESTED_END		SOLVABLE_ENHANCES

#define SOLV_ERROR_NOT_SOLV	1
#define SOLV_ERROR_UNSUPPORTED	2
#define SOLV_ERROR_EOF		3
#define SOLV_ERROR_ID_RANGE	4
#define SOLV_ERROR_OVERFLOW	5
#define SOLV_ERROR_CORRUPT	6



/*******************************************************************************
 * functions to extract data from a file handle
 */

/*
 * read u32
 */

static unsigned int
read_u32(Repodata *data)
{
  int c, i;
  unsigned int x = 0;

  if (data->error)
    return 0;
  for (i = 0; i < 4; i++)
    {
      c = getc(data->fp);
      if (c == EOF)
	{
	  pool_debug(data->repo->pool, SOLV_ERROR, "unexpected EOF\n");
	  data->error = SOLV_ERROR_EOF;
	  return 0;
	}
      x = (x << 8) | c;
    }
  return x;
}


/*
 * read u8
 */

static unsigned int
read_u8(Repodata *data)
{
  int c;

  if (data->error)
    return 0;
  c = getc(data->fp);
  if (c == EOF)
    {
      pool_debug(data->repo->pool, SOLV_ERROR, "unexpected EOF\n");
      data->error = SOLV_ERROR_EOF;
      return 0;
    }
  return c;
}


/*
 * read Id
 */

static Id
read_id(Repodata *data, Id max)
{
  unsigned int x = 0;
  int c, i;

  if (data->error)
    return 0;
  for (i = 0; i < 5; i++)
    {
      c = getc(data->fp);
      if (c == EOF)
	{
          pool_debug(data->repo->pool, SOLV_ERROR, "unexpected EOF\n");
	  data->error = SOLV_ERROR_EOF;
	  return 0;
	}
      if (!(c & 128))
	{
	  x = (x << 7) | c;
	  if (max && x >= max)
	    {
              pool_debug(data->repo->pool, SOLV_ERROR, "read_id: id too large (%u/%u)\n", x, max);
	      data->error = SOLV_ERROR_ID_RANGE;
	      return 0;
	    }
	  return x;
	}
      x = (x << 7) ^ c ^ 128;
    }
  pool_debug(data->repo->pool, SOLV_ERROR, "read_id: id too long\n");
  data->error = SOLV_ERROR_CORRUPT;
  return 0;
}


static Id *
read_idarray(Repodata *data, Id max, Id *map, Id *store, Id *end)
{
  unsigned int x = 0;
  int c;

  if (data->error)
    return 0;
  for (;;)
    {
      c = getc(data->fp);
      if (c == EOF)
	{
	  pool_debug(data->repo->pool, SOLV_ERROR, "unexpected EOF\n");
	  data->error = SOLV_ERROR_EOF;
	  return 0;
	}
      if ((c & 128) != 0)
	{
	  x = (x << 7) ^ c ^ 128;
	  continue;
	}
      x = (x << 6) | (c & 63);
      if (max && x >= max)
	{
	  pool_debug(data->repo->pool, SOLV_ERROR, "read_idarray: id too large (%u/%u)\n", x, max);
	  data->error = SOLV_ERROR_ID_RANGE;
	  return 0;
	}
      if (map)
	x = map[x];
      if (store == end)
	{
	  pool_debug(data->repo->pool, SOLV_ERROR, "read_idarray: array overflow\n");
	  return 0;
	}
      *store++ = x;
      if ((c & 64) == 0)
	{
	  if (x == 0)	/* already have trailing zero? */
	    return store;
	  if (store == end)
	    {
	      pool_debug(data->repo->pool, SOLV_ERROR, "read_idarray: array overflow\n");
	      data->error = SOLV_ERROR_OVERFLOW;
	      return 0;
	    }
	  *store++ = 0;
	  return store;
	}
      x = 0;
    }
}


/*******************************************************************************
 * functions to extract data from memory
 */

/*
 * read array of Ids
 */

static inline unsigned char *
data_read_id_max(unsigned char *dp, Id *ret, Id *map, int max, Repodata *data)
{
  Id x;
  dp = data_read_id(dp, &x);
  if (x < 0 || (max && x >= max))
    {
      pool_debug(data->repo->pool, SOLV_ERROR, "data_read_id_max: id too large (%u/%u)\n", x, max);
      data->error = SOLV_ERROR_ID_RANGE;
      x = 0;
    }
  *ret = map ? map[x] : x;
  return dp;
}

static unsigned char *
data_read_idarray(unsigned char *dp, Id **storep, Id *map, int max, Repodata *data)
{
  Id *store = *storep;
  unsigned int x = 0;
  int c;

  for (;;)
    {
      c = *dp++;
      if ((c & 128) != 0)
	{
	  x = (x << 7) ^ c ^ 128;
	  continue;
	}
      x = (x << 6) | (c & 63);
      if (max && x >= max)
	{
	  pool_debug(data->repo->pool, SOLV_ERROR, "data_read_idarray: id too large (%u/%u)\n", x, max);
	  data->error = SOLV_ERROR_ID_RANGE;
	  break;
	}
      *store++ = x;
      if ((c & 64) == 0)
        break;
      x = 0;
    }
  *store++ = 0;
  *storep = store;
  return dp;
}

static unsigned char *
data_read_rel_idarray(unsigned char *dp, Id **storep, Id *map, int max, Repodata *data, Id marker)
{
  Id *store = *storep;
  Id old = 0;
  unsigned int x = 0;
  int c;

  for (;;)
    {
      c = *dp++;
      if ((c & 128) != 0)
	{
	  x = (x << 7) ^ c ^ 128;
	  continue;
	}
      x = (x << 6) | (c & 63);
      if (x == 0)
	{
	  if (!(c & 64))
	    break;
          if (marker)
	    *store++ = marker;
	  old = 0;
	  continue;
	}
      x = old + (x - 1);
      old = x;
      if (max && x >= max)
	{
	  pool_debug(data->repo->pool, SOLV_ERROR, "data_read_rel_idarray: id too large (%u/%u)\n", x, max);
	  data->error = SOLV_ERROR_ID_RANGE;
	  break;
	}
      *store++ = map ? map[x] : x;
      if (!(c & 64))
        break;
      x = 0;
    }
  *store++ = 0;
  *storep = store;
  return dp;
}




/*******************************************************************************
 * functions to add data to our incore memory space
 */

#define INCORE_ADD_CHUNK 8192
#define DATA_READ_CHUNK 8192

static void
incore_add_id(Repodata *data, Id sx)
{
  unsigned int x = (unsigned int)sx;
  unsigned char *dp;
  /* make sure we have at least 5 bytes free */
  if (data->incoredatafree < 5)
    {
      data->incoredata = solv_realloc(data->incoredata, data->incoredatalen + INCORE_ADD_CHUNK);
      data->incoredatafree = INCORE_ADD_CHUNK;
    }
  dp = data->incoredata + data->incoredatalen;
  if (x >= (1 << 14))
    {
      if (x >= (1 << 28))
	*dp++ = (x >> 28) | 128;
      if (x >= (1 << 21))
	*dp++ = (x >> 21) | 128;
      *dp++ = (x >> 14) | 128;
    }
  if (x >= (1 << 7))
    *dp++ = (x >> 7) | 128;
  *dp++ = x & 127;
  data->incoredatafree -= dp - (data->incoredata + data->incoredatalen);
  data->incoredatalen = dp - data->incoredata;
}

static void
incore_add_ideof(Repodata *data, Id sx, int eof)
{
  unsigned int x = (unsigned int)sx;
  unsigned char *dp;
  /* make sure we have at least 5 bytes free */
  if (data->incoredatafree < 5)
    {
      data->incoredata = solv_realloc(data->incoredata, data->incoredatalen + INCORE_ADD_CHUNK);
      data->incoredatafree = INCORE_ADD_CHUNK;
    }
  dp = data->incoredata + data->incoredatalen;
  if (x >= (1 << 13))
    {
      if (x >= (1 << 27))
	*dp++ = (x >> 27) | 128;
      if (x >= (1 << 20))
	*dp++ = (x >> 20) | 128;
      *dp++ = (x >> 13) | 128;
    }
  if (x >= (1 << 6))
    *dp++ = (x >> 6) | 128;
  *dp++ = eof ? (x & 63) : (x & 63) | 64;
  data->incoredatafree -= dp - (data->incoredata + data->incoredatalen);
  data->incoredatalen = dp - data->incoredata;
}

static void
incore_add_blob(Repodata *data, unsigned char *buf, int len)
{
  if (data->incoredatafree < len)
    {
      data->incoredata = solv_realloc(data->incoredata, data->incoredatalen + INCORE_ADD_CHUNK + len);
      data->incoredatafree = INCORE_ADD_CHUNK + len;
    }
  memcpy(data->incoredata + data->incoredatalen, buf, len);
  data->incoredatafree -= len;
  data->incoredatalen += len;
}

static void
incore_map_idarray(Repodata *data, unsigned char *dp, Id *map, Id max)
{
  /* We have to map the IDs, which might also change
     the necessary number of bytes, so we can't just copy
     over the blob and adjust it.  */
  for (;;)
    {
      Id id;
      int eof;
      dp = data_read_ideof(dp, &id, &eof);
      if (id < 0 || (max && id >= max))
	{
	  pool_debug(data->repo->pool, SOLV_ERROR, "incore_map_idarray: id too large (%u/%u)\n", id, max);
	  data->error = SOLV_ERROR_ID_RANGE;
	  break;
	}
      id = map[id];
      incore_add_ideof(data, id, eof);
      if (eof)
	break;
    }
}

#if 0
static void
incore_add_u32(Repodata *data, unsigned int x)
{
  unsigned char *dp;
  /* make sure we have at least 4 bytes free */
  if (data->incoredatafree < 4)
    {
      data->incoredata = solv_realloc(data->incoredata, data->incoredatalen + INCORE_ADD_CHUNK);
      data->incoredatafree = INCORE_ADD_CHUNK;
    }
  dp = data->incoredata + data->incoredatalen;
  *dp++ = x >> 24;
  *dp++ = x >> 16;
  *dp++ = x >> 8;
  *dp++ = x;
  data->incoredatafree -= 4;
  data->incoredatalen += 4;
}

static void
incore_add_u8(Repodata *data, unsigned int x)
{
  unsigned char *dp;
  /* make sure we have at least 1 byte free */
  if (data->incoredatafree < 1)
    {
      data->incoredata = solv_realloc(data->incoredata, data->incoredatalen + 1024);
      data->incoredatafree = 1024;
    }
  dp = data->incoredata + data->incoredatalen;
  *dp++ = x;
  data->incoredatafree--;
  data->incoredatalen++;
}
#endif


/*******************************************************************************
 * our main function
 */

/*
 * read repo from .solv file and add it to pool
 */

int
repo_add_solv(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  int i, l;
  unsigned int numid, numrel, numdir, numsolv;
  unsigned int numkeys, numschemata;

  Offset sizeid;
  Offset *str;			       /* map Id -> Offset into string space */
  char *strsp;			       /* repo string space */
  char *sp;			       /* pointer into string space */
  Id *idmap;			       /* map of repo Ids to pool Ids */
  Id id, type;
  unsigned int hashmask, h;
  int hh;
  Id *hashtbl;
  Id name, evr, did;
  int relflags;
  Reldep *ran;
  unsigned int size_idarray;
  Id *idarraydatap, *idarraydataend;
  Offset ido;
  Solvable *s;
  unsigned int solvflags;
  unsigned int solvversion;
  Repokey *keys;
  Id *schemadata, *schemadatap, *schemadataend;
  Id *schemata, key, *keyp;
  int nentries;
  int have_xdata;
  int maxsize, allsize;
  unsigned char *buf, *bufend, *dp, *dps;
  Id stack[3 * 5];
  int keydepth;
  int needchunk;	/* need a new chunk of data */
  unsigned int now;
  int oldnstrings = pool->ss.nstrings;
  int oldnrels = pool->nrels;

  struct _Stringpool *spool;

  Repodata *parent = 0;
  Repodata data;

  now = solv_timems(0);

  if ((flags & REPO_USE_LOADING) != 0)
    {
      /* this is a stub replace operation */
      flags |= REPO_EXTEND_SOLVABLES;
      /* use REPO_REUSE_REPODATA hack so that the old repodata is kept */
      parent = repo_add_repodata(repo, flags | REPO_REUSE_REPODATA);
    }
    
  memset(&data, 0, sizeof(data));
  data.repo = repo;
  data.fp = fp;
  repopagestore_init(&data.store);

  if (read_u32(&data) != ('S' << 24 | 'O' << 16 | 'L' << 8 | 'V'))
    {
      pool_debug(pool, SOLV_ERROR, "not a SOLV file\n");
      return SOLV_ERROR_NOT_SOLV;
    }
  solvversion = read_u32(&data);
  switch (solvversion)
    {
      case SOLV_VERSION_8:
	break;
      default:
        pool_debug(pool, SOLV_ERROR, "unsupported SOLV version\n");
        return SOLV_ERROR_UNSUPPORTED;
    }

  numid = read_u32(&data);
  numrel = read_u32(&data);
  numdir = read_u32(&data);
  numsolv = read_u32(&data);
  numkeys = read_u32(&data);
  numschemata = read_u32(&data);
  solvflags = read_u32(&data);

  if (numdir && numdir < 2)
    {
      pool_debug(pool, SOLV_ERROR, "bad number of dirs\n");
      return SOLV_ERROR_CORRUPT;
    }

  if (numrel && (flags & REPO_LOCALPOOL) != 0)
    {
      pool_debug(pool, SOLV_ERROR, "relations are forbidden in a local pool\n");
      return SOLV_ERROR_CORRUPT;
    }
  if (parent && numsolv)
    {
      /* make sure that we exactly replace the stub repodata */
      if (parent->end - parent->start != numsolv)
	{
	  pool_debug(pool, SOLV_ERROR, "sub-repository solvable number does not match main repository (%d - %d)\n", parent->end - parent->start, numsolv);
	  return SOLV_ERROR_CORRUPT;
	}
      for (i = 0; i < numsolv; i++)
	if (pool->solvables[parent->start + i].repo != repo)
	  {
	    pool_debug(pool, SOLV_ERROR, "main repository contains holes\n");
	    return SOLV_ERROR_CORRUPT;
	  }
    }

  /*******  Part 1: string IDs  *****************************************/

  sizeid = read_u32(&data);	       /* size of string+Id space */

  /*
   * read strings and Ids
   * 
   */

  
  /*
   * alloc buffers
   */

  if (!(flags & REPO_LOCALPOOL))
    spool = &pool->ss;
  else
    {
      data.localpool = 1;
      spool = &data.spool;
      spool->stringspace = solv_malloc(7);
      strcpy(spool->stringspace, "<NULL>");
      spool->sstrings = 7;
      spool->nstrings = 0;
    }

  /* alloc string buffer */
  spool->stringspace = solv_realloc(spool->stringspace, spool->sstrings + sizeid + 1);
  /* alloc string offsets (Id -> Offset into string space) */
  spool->strings = solv_realloc2(spool->strings, spool->nstrings + numid, sizeof(Offset));

  strsp = spool->stringspace;
  str = spool->strings;		       /* array of offsets into strsp, indexed by Id */

  /* point to _BEHIND_ already allocated string/Id space */
  strsp += spool->sstrings;


  /*
   * read new repo at end of pool
   */
  
  if ((solvflags & SOLV_FLAG_PREFIX_POOL) == 0)
    {
      if (sizeid && fread(strsp, sizeid, 1, fp) != 1)
	{
	  pool_debug(pool, SOLV_ERROR, "read error while reading strings\n");
	  return SOLV_ERROR_EOF;
	}
    }
  else
    {
      unsigned int pfsize = read_u32(&data);
      char *prefix = solv_malloc(pfsize);
      char *pp = prefix;
      char *old_str = 0;
      char *dest = strsp;
      int freesp = sizeid;

      if (pfsize && fread(prefix, pfsize, 1, fp) != 1)
        {
	  pool_debug(pool, SOLV_ERROR, "read error while reading strings\n");
	  solv_free(prefix);
	  return SOLV_ERROR_EOF;
	}
      for (i = 1; i < numid; i++)
        {
	  int same = (unsigned char)*pp++;
	  size_t len = strlen(pp) + 1;
	  freesp -= same + len;
	  if (freesp < 0)
	    {
	      pool_debug(pool, SOLV_ERROR, "overflow while expanding strings\n");
	      solv_free(prefix);
	      return SOLV_ERROR_OVERFLOW;
	    }
	  if (same)
	    memcpy(dest, old_str, same);
	  memcpy(dest + same, pp, len);
	  pp += len;
	  old_str = dest;
	  dest += same + len;
	}
      solv_free(prefix);
      if (freesp != 0)
	{
	  pool_debug(pool, SOLV_ERROR, "expanding strings size mismatch\n");
	  return SOLV_ERROR_CORRUPT;
	}
    }
  strsp[sizeid] = 0;		       /* make string space \0 terminated */
  sp = strsp;

  if ((flags & REPO_LOCALPOOL) != 0)
    {
      /* no shared pool, thus no idmap and no unification */
      idmap = 0;
      spool->nstrings = numid;
      str[0] = 0;
      if (*sp)
	{
	  /* we need the '' for directories */
	  pool_debug(pool, SOLV_ERROR, "store strings don't start with ''\n");
	  return SOLV_ERROR_CORRUPT;
	}
      for (i = 1; i < spool->nstrings; i++)
	{
	  if (sp >= strsp + sizeid)
	    {
	      pool_debug(pool, SOLV_ERROR, "not enough strings\n");
	      return SOLV_ERROR_OVERFLOW;
	    }
	  str[i] = sp - spool->stringspace;
	  sp += strlen(sp) + 1;
	}
      spool->sstrings = sp - spool->stringspace;
    }
  else
    {
      /* alloc id map for name and rel Ids. this maps ids in the solv files
       * to the ids in our pool */
      idmap = solv_calloc(numid + numrel, sizeof(Id));

      /* grow hash if needed, otherwise reuse */
      hashmask = mkmask(spool->nstrings + numid);
#if 0
      POOL_DEBUG(SOLV_DEBUG_STATS, "read %d strings\n", numid);
      POOL_DEBUG(SOLV_DEBUG_STATS, "string hash buckets: %d, old %d\n", hashmask + 1, spool->stringhashmask + 1);
#endif
      if (hashmask > spool->stringhashmask)
	{
	  spool->stringhashtbl = solv_free(spool->stringhashtbl);
	  spool->stringhashmask = hashmask;
          spool->stringhashtbl = hashtbl = solv_calloc(hashmask + 1, sizeof(Id));
	  for (i = 1; i < spool->nstrings; i++)
	    {
	      h = strhash(spool->stringspace + spool->strings[i]) & hashmask;
	      hh = HASHCHAIN_START;
	      while (hashtbl[h])
		h = HASHCHAIN_NEXT(h, hh, hashmask);
	      hashtbl[h] = i;
	    }
	}
      else
	{
	  hashtbl = spool->stringhashtbl;
	  hashmask = spool->stringhashmask;
	}

      /*
       * run over strings and merge with pool.
       * also populate id map (maps solv Id -> pool Id)
       */
      for (i = 1; i < numid; i++)
	{
	  if (sp >= strsp + sizeid)
	    {
	      solv_free(hashtbl);
	      solv_free(idmap);
	      pool_debug(pool, SOLV_ERROR, "not enough strings %d %d\n", i, numid);
	      return SOLV_ERROR_OVERFLOW;
	    }
	  if (!*sp)			       /* empty string */
	    {
	      idmap[i] = ID_EMPTY;
	      sp++;
	      continue;
	    }

	  /* find hash slot */
	  h = strhash(sp) & hashmask;
	  hh = HASHCHAIN_START;
	  for (;;)
	    {
	      id = hashtbl[h];
	      if (!id)
		break;
	      if (!strcmp(spool->stringspace + spool->strings[id], sp))
		break;		/* already in pool */
	      h = HASHCHAIN_NEXT(h, hh, hashmask);
	    }

	  /* length == offset to next string */
	  l = strlen(sp) + 1;
	  if (!id)	       /* end of hash chain -> new string */
	    {
	      id = spool->nstrings++;
	      hashtbl[h] = id;
	      str[id] = spool->sstrings;	/* save offset */
	      if (sp != spool->stringspace + spool->sstrings)
		memmove(spool->stringspace + spool->sstrings, sp, l);
	      spool->sstrings += l;
	    }
	  idmap[i] = id;       /* repo relative -> pool relative */
	  sp += l;	       /* next string */
	}
      if (hashmask > mkmask(spool->nstrings + 8192))
	{
	  spool->stringhashtbl = solv_free(spool->stringhashtbl);
	  spool->stringhashmask = 0;
	}
    }
  pool_shrink_strings(pool);	       /* vacuum */

  
  /*******  Part 2: Relation IDs  ***************************************/

  /*
   * read RelDeps
   * 
   */
  
  if (numrel)
    {
      /* extend rels */
      pool->rels = solv_realloc2(pool->rels, pool->nrels + numrel, sizeof(Reldep));
      ran = pool->rels;

      /* grow hash if needed, otherwise reuse */
      hashmask = mkmask(pool->nrels + numrel);
#if 0
      POOL_DEBUG(SOLV_DEBUG_STATS, "read %d rels\n", numrel);
      POOL_DEBUG(SOLV_DEBUG_STATS, "rel hash buckets: %d, old %d\n", hashmask + 1, pool->relhashmask + 1);
#endif
      if (hashmask > pool->relhashmask)
	{
	  pool->relhashtbl = solv_free(pool->relhashtbl);
	  pool->relhashmask = hashmask;
          pool->relhashtbl = hashtbl = solv_calloc(hashmask + 1, sizeof(Id));
	  for (i = 1; i < pool->nrels; i++)
	    {
	      h = relhash(ran[i].name, ran[i].evr, ran[i].flags) & hashmask;
	      hh = HASHCHAIN_START;
	      while (hashtbl[h])
		h = HASHCHAIN_NEXT(h, hh, hashmask);
	      hashtbl[h] = i;
	    }
	}
      else
	{
	  hashtbl = pool->relhashtbl;
	  hashmask = pool->relhashmask;
	}

      /*
       * read RelDeps from repo
       */
      for (i = 0; i < numrel; i++)
	{
	  name = read_id(&data, i + numid);	/* read (repo relative) Ids */
	  evr = read_id(&data, i + numid);
	  relflags = read_u8(&data);
	  name = idmap[name];		/* map to (pool relative) Ids */
	  evr = idmap[evr];
	  h = relhash(name, evr, relflags) & hashmask;
	  hh = HASHCHAIN_START;
	  for (;;)
	    {
	      id = hashtbl[h];
	      if (!id)		/* end of hash chain reached */
		break;
	      if (ran[id].name == name && ran[id].evr == evr && ran[id].flags == relflags)
		break;
	      h = HASHCHAIN_NEXT(h, hh, hashmask);
	    }
	  if (!id)		/* new RelDep */
	    {
	      id = pool->nrels++;
	      hashtbl[h] = id;
	      ran[id].name = name;
	      ran[id].evr = evr;
	      ran[id].flags = relflags;
	    }
	  idmap[i + numid] = MAKERELDEP(id);   /* fill Id map */
	}
      if (hashmask > mkmask(pool->nrels + 4096))
	{
	  pool->relhashtbl = solv_free(pool->relhashtbl);
	  pool->relhashmask = 0;
	}
      pool_shrink_rels(pool);		/* vacuum */
    }

  /* if we added ids/rels, make room in our whatprovide arrays */
  if (!(flags & REPO_LOCALPOOL))
    {
      if (pool->whatprovides && oldnstrings != pool->ss.nstrings)
	{
	  int newlen = (pool->ss.nstrings + WHATPROVIDES_BLOCK) & ~WHATPROVIDES_BLOCK;
	  pool->whatprovides = solv_realloc2(pool->whatprovides, newlen, sizeof(Offset));
	  memset(pool->whatprovides + oldnstrings, 0, (newlen - oldnstrings) * sizeof(Offset));
	}
      if (pool->whatprovides_rel && oldnrels != pool->nrels)
	{
	  int newlen = (pool->nrels + WHATPROVIDES_BLOCK) & ~WHATPROVIDES_BLOCK;
	  pool->whatprovides_rel = solv_realloc2(pool->whatprovides_rel, newlen, sizeof(Offset));
	  memset(pool->whatprovides_rel + oldnrels, 0, (newlen - oldnrels) * sizeof(Offset));
	}
    }

  /*******  Part 3: Dirs  ***********************************************/
  if (numdir)
    {
      data.dirpool.dirs = solv_malloc2(numdir, sizeof(Id));
      data.dirpool.ndirs = numdir;
      data.dirpool.dirs[0] = 0;		/* dir 0: virtual root */
      data.dirpool.dirs[1] = 1;		/* dir 1: / */
      for (i = 2; i < numdir; i++)
	{
	  id = read_id(&data, i + numid);
	  if (id >= numid)
	    data.dirpool.dirs[i] = -(id - numid);
	  else if (idmap)
	    data.dirpool.dirs[i] = idmap[id];
	  else
	    data.dirpool.dirs[i] = id;
	}
    }

  /*******  Part 4: Keys  ***********************************************/

  keys = solv_calloc(numkeys, sizeof(*keys));
  /* keys start at 1 */
  for (i = 1; i < numkeys; i++)
    {
      id = read_id(&data, numid);
      if (idmap)
	id = idmap[id];
      else if ((flags & REPO_LOCALPOOL) != 0)
        id = pool_str2id(pool, stringpool_id2str(spool, id), 1);
      type = read_id(&data, numid);
      if (idmap)
	type = idmap[type];
      else if ((flags & REPO_LOCALPOOL) != 0)
        type = pool_str2id(pool, stringpool_id2str(spool, type), 1);
      if (type < REPOKEY_TYPE_VOID || type > REPOKEY_TYPE_FLEXARRAY)
	{
	  pool_debug(pool, SOLV_ERROR, "unsupported data type '%s'\n", pool_id2str(pool, type));
	  data.error = SOLV_ERROR_UNSUPPORTED;
	  type = REPOKEY_TYPE_VOID;
	}
      keys[i].name = id;
      keys[i].type = type;
      keys[i].size = read_id(&data, keys[i].type == REPOKEY_TYPE_CONSTANTID ? numid + numrel : 0);
      keys[i].storage = read_id(&data, 0);
      /* old versions used SOLVABLE for main solvable data */
      if (keys[i].storage == KEY_STORAGE_SOLVABLE)
	keys[i].storage = KEY_STORAGE_INCORE;
      if (keys[i].storage != KEY_STORAGE_INCORE && keys[i].storage != KEY_STORAGE_VERTICAL_OFFSET)
	{
	  pool_debug(pool, SOLV_ERROR, "unsupported storage type %d\n", keys[i].storage);
	  data.error = SOLV_ERROR_UNSUPPORTED;
	}
      if (id >= SOLVABLE_NAME && id <= RPM_RPMDBID)
	{
	  if (keys[i].storage != KEY_STORAGE_INCORE)
	    {
	      pool_debug(pool, SOLV_ERROR, "main solvable data must use incore storage%d\n", keys[i].storage);
	      data.error = SOLV_ERROR_UNSUPPORTED;
	    }
	  keys[i].storage = KEY_STORAGE_SOLVABLE;
	}
      /* cannot handle rel idarrays in incore/vertical */
      if (type == REPOKEY_TYPE_REL_IDARRAY && keys[i].storage != KEY_STORAGE_SOLVABLE)
	{
	  pool_debug(pool, SOLV_ERROR, "type REL_IDARRAY is only supported for STORAGE_SOLVABLE\n");
	  data.error = SOLV_ERROR_UNSUPPORTED;
	}
      /* cannot handle mapped ids in vertical */
      if (!(flags & REPO_LOCALPOOL) && keys[i].storage == KEY_STORAGE_VERTICAL_OFFSET && (type == REPOKEY_TYPE_ID || type == REPOKEY_TYPE_IDARRAY))
	{
	  pool_debug(pool, SOLV_ERROR, "mapped ids are not supported for STORAGE_VERTICAL_OFFSET\n");
	  data.error = SOLV_ERROR_UNSUPPORTED;
	}
 
      if (keys[i].type == REPOKEY_TYPE_CONSTANTID && idmap)
	keys[i].size = idmap[keys[i].size];
#if 0
      fprintf(stderr, "key %d %s %s %d %d\n", i, pool_id2str(pool,id), pool_id2str(pool, keys[i].type),
               keys[i].size, keys[i].storage);
#endif
    }

  have_xdata = parent ? 1 : 0;
  for (i = 1; i < numkeys; i++)
    if (keys[i].storage == KEY_STORAGE_INCORE || keys[i].storage == KEY_STORAGE_VERTICAL_OFFSET)
      have_xdata = 1;

  data.keys = keys;
  data.nkeys = numkeys;
  for (i = 1; i < numkeys; i++)
    {
      id = keys[i].name;
      data.keybits[(id >> 3) & (sizeof(data.keybits) - 1)] |= 1 << (id & 7);
    }

  /*******  Part 5: Schemata ********************************************/
  
  id = read_id(&data, 0);
  schemadata = solv_calloc(id + 1, sizeof(Id));
  schemadatap = schemadata + 1;
  schemadataend = schemadatap + id;
  schemata = solv_calloc(numschemata, sizeof(Id));
  for (i = 1; i < numschemata; i++)
    {
      schemata[i] = schemadatap - schemadata;
      schemadatap = read_idarray(&data, numid, 0, schemadatap, schemadataend);
#if 0
      Id *sp = schemadata + schemata[i];
      fprintf(stderr, "schema %d:", i);
      for (; *sp; sp++)
        fprintf(stderr, " %d", *sp);
      fprintf(stderr, "\n");
#endif
    }
  data.schemata = schemata;
  data.nschemata = numschemata;
  data.schemadata = schemadata;
  data.schemadatalen = schemadataend - data.schemadata;

  /*******  Part 6: Data ********************************************/

  idarraydatap = idarraydataend = 0;
  size_idarray = 0;

  maxsize = read_id(&data, 0);
  allsize = read_id(&data, 0);
  maxsize += 5;	/* so we can read the next schema of an array */
  if (maxsize > allsize)
    maxsize = allsize;

  buf = solv_calloc(maxsize + DATA_READ_CHUNK + 4, 1);	/* 4 extra bytes to detect overflows */
  bufend = buf;
  dp = buf;

  l = maxsize;
  if (l < DATA_READ_CHUNK)
    l = DATA_READ_CHUNK;
  if (l > allsize)
    l = allsize;
  if (!l || fread(buf, l, 1, data.fp) != 1)
    {
      pool_debug(pool, SOLV_ERROR, "unexpected EOF\n");
      data.error = SOLV_ERROR_EOF;
      id = 0;
    }
  else
    {
      bufend = buf + l;
      allsize -= l;
      dp = data_read_id_max(dp, &id, 0, numschemata, &data);
    }

  incore_add_id(&data, 0);	/* XXX? */
  incore_add_id(&data, id);
  keyp = schemadata + schemata[id];
  data.mainschema = id;
  for (i = 0; keyp[i]; i++)
    ;
  if (i)
    data.mainschemaoffsets = solv_calloc(i, sizeof(Id));

  nentries = 0;
  keydepth = 0;
  s = 0;
  needchunk = 1;
  for(;;)
    {
      /* make sure we have enough room */
      if (keydepth == 0 || needchunk)
	{
	  int left = bufend - dp;
	  /* read data chunk to dp */
	  if (data.error)
	    break;
	  if (left < 0)
	    {
	      pool_debug(pool, SOLV_ERROR, "buffer overrun\n");
	      data.error = SOLV_ERROR_EOF;
	      break;
	    }
	  if (left < maxsize)
	    {
	      if (left)
		memmove(buf, dp, left);
	      l = maxsize - left;
	      if (l < DATA_READ_CHUNK)
		l = DATA_READ_CHUNK;
	      if (l > allsize)
		l = allsize;
	      if (l && fread(buf + left, l, 1, data.fp) != 1)
		{
		  pool_debug(pool, SOLV_ERROR, "unexpected EOF\n");
		  data.error = SOLV_ERROR_EOF;
		  break;
		}
	      allsize -= l;
	      left += l;
	      bufend = buf + left;
	      if (allsize + left < maxsize)
		maxsize = allsize + left;
	      dp = buf;
	    }
	  needchunk = 0;
	}

      key = *keyp++;
#if 0
printf("key %d at %d\n", key, (int)(keyp - 1 - schemadata));
#endif
      if (!key)
	{
	  if (keydepth <= 3)
	    needchunk = 1;
	  if (nentries)
	    {
	      if (s && keydepth == 3)
		{
		  s++;	/* next solvable */
	          if (have_xdata)
		    data.incoreoffset[(s - pool->solvables) - data.start] = data.incoredatalen;
		}
	      id = stack[keydepth - 1];
	      if (!id)
		{
		  dp = data_read_id_max(dp, &id, 0, numschemata, &data);
		  incore_add_id(&data, id);
		}
	      keyp = schemadata + schemata[id];
	      nentries--;
	      continue;
	    }
	  if (!keydepth)
	    break;
	  --keydepth;
	  keyp = schemadata + stack[--keydepth];
	  nentries = stack[--keydepth];
#if 0
printf("pop flexarray %d %d\n", keydepth, nentries);
#endif
	  if (!keydepth && s)
	    s = 0;	/* back from solvables */
	  continue;
	}

      if (keydepth == 0)
	data.mainschemaoffsets[keyp - 1 - (schemadata + schemata[data.mainschema])] = data.incoredatalen;

#if 0
printf("=> %s %s %p\n", pool_id2str(pool, keys[key].name), pool_id2str(pool, keys[key].type), s);
#endif
      id = keys[key].name;
      if (keys[key].storage == KEY_STORAGE_VERTICAL_OFFSET)
	{
	  dps = dp;
	  dp = data_skip(dp, REPOKEY_TYPE_ID);
	  dp = data_skip(dp, REPOKEY_TYPE_ID);
	  incore_add_blob(&data, dps, dp - dps);	/* just record offset/size */
	  continue;
	}
      switch (keys[key].type)
	{
	case REPOKEY_TYPE_ID:
	  dp = data_read_id_max(dp, &did, idmap, numid + numrel, &data);
	  if (s && id == SOLVABLE_NAME)
	    s->name = did; 
	  else if (s && id == SOLVABLE_ARCH)
	    s->arch = did; 
	  else if (s && id == SOLVABLE_EVR)
	    s->evr = did; 
	  else if (s && id == SOLVABLE_VENDOR)
	    s->vendor = did; 
	  else if (keys[key].storage == KEY_STORAGE_INCORE)
	    incore_add_id(&data, did);
#if 0
	  POOL_DEBUG(SOLV_DEBUG_STATS, "%s -> %s\n", pool_id2str(pool, id), pool_id2str(pool, did));
#endif
	  break;
	case REPOKEY_TYPE_IDARRAY:
	case REPOKEY_TYPE_REL_IDARRAY:
	  if (!s || id < INTERESTED_START || id > INTERESTED_END)
	    {
	      dps = dp;
	      dp = data_skip(dp, REPOKEY_TYPE_IDARRAY);
	      if (keys[key].storage != KEY_STORAGE_INCORE)
		break;
	      if (idmap)
		incore_map_idarray(&data, dps, idmap, numid + numrel);
	      else
		incore_add_blob(&data, dps, dp - dps);
	      break;
	    }
	  ido = idarraydatap - repo->idarraydata;
	  if (keys[key].type == REPOKEY_TYPE_IDARRAY)
	    dp = data_read_idarray(dp, &idarraydatap, idmap, numid + numrel, &data);
	  else if (id == SOLVABLE_REQUIRES)
	    dp = data_read_rel_idarray(dp, &idarraydatap, idmap, numid + numrel, &data, SOLVABLE_PREREQMARKER);
	  else if (id == SOLVABLE_PROVIDES)
	    dp = data_read_rel_idarray(dp, &idarraydatap, idmap, numid + numrel, &data, SOLVABLE_FILEMARKER);
	  else
	    dp = data_read_rel_idarray(dp, &idarraydatap, idmap, numid + numrel, &data, 0);
	  if (idarraydatap > idarraydataend)
	    {
	      pool_debug(pool, SOLV_ERROR, "idarray overflow\n");
	      data.error = SOLV_ERROR_OVERFLOW;
	      break;
	    }
	  if (id == SOLVABLE_PROVIDES)
	    s->provides = ido;
	  else if (id == SOLVABLE_OBSOLETES)
	    s->obsoletes = ido;
	  else if (id == SOLVABLE_CONFLICTS)
	    s->conflicts = ido;
	  else if (id == SOLVABLE_REQUIRES)
	    s->requires = ido;
	  else if (id == SOLVABLE_RECOMMENDS)
	    s->recommends= ido;
	  else if (id == SOLVABLE_SUPPLEMENTS)
	    s->supplements = ido;
	  else if (id == SOLVABLE_SUGGESTS)
	    s->suggests = ido;
	  else if (id == SOLVABLE_ENHANCES)
	    s->enhances = ido;
#if 0
	  POOL_DEBUG(SOLV_DEBUG_STATS, "%s ->\n", pool_id2str(pool, id));
	  for (; repo->idarraydata[ido]; ido++)
	    POOL_DEBUG(SOLV_DEBUG_STATS,"  %s\n", pool_dep2str(pool, repo->idarraydata[ido]));
#endif
	  break;
	case REPOKEY_TYPE_FIXARRAY:
	case REPOKEY_TYPE_FLEXARRAY:
	  if (!keydepth)
	    needchunk = 1;
          if (keydepth == sizeof(stack)/sizeof(*stack))
	    {
	      pool_debug(pool, SOLV_ERROR, "array stack overflow\n");
	      data.error = SOLV_ERROR_CORRUPT;
	      break;
	    }
	  stack[keydepth++] = nentries;
	  stack[keydepth++] = keyp - schemadata;
	  stack[keydepth++] = 0;
	  dp = data_read_id_max(dp, &nentries, 0, 0, &data);
	  incore_add_id(&data, nentries);
	  if (!nentries)
	    {
	      /* zero size array? */
	      keydepth -= 2;
	      nentries = stack[--keydepth];
	      break;
	    }
	  if (keydepth == 3 && id == REPOSITORY_SOLVABLES)
	    {
	      /* horray! here come the solvables */
	      if (nentries != numsolv)
		{
		  pool_debug(pool, SOLV_ERROR, "inconsistent number of solvables: %d %d\n", nentries, numsolv);
		  data.error = SOLV_ERROR_CORRUPT;
		  break;
		}
	      if (idarraydatap)
		{
		  pool_debug(pool, SOLV_ERROR, "more than one solvable block\n");
		  data.error = SOLV_ERROR_CORRUPT;
		  break;
		}
	      if (parent)
		s = pool_id2solvable(pool, parent->start);
	      else
		s = pool_id2solvable(pool, repo_add_solvable_block(repo, numsolv));
	      data.start = s - pool->solvables;
	      data.end = data.start + numsolv;
	      repodata_extend_block(&data, data.start, numsolv);
	      for (i = 1; i < numkeys; i++)
		{
		  id = keys[i].name;
		  if ((keys[i].type == REPOKEY_TYPE_IDARRAY || keys[i].type == REPOKEY_TYPE_REL_IDARRAY)
		      && id >= INTERESTED_START && id <= INTERESTED_END)
		    size_idarray += keys[i].size;
		}
	      /* allocate needed space in repo */
	      /* we add maxsize because it is an upper limit for all idarrays, thus we can't overflow */
	      repo_reserve_ids(repo, 0, size_idarray + maxsize + 1);
	      idarraydatap = repo->idarraydata + repo->idarraysize;
	      repo->idarraysize += size_idarray;
	      idarraydataend = idarraydatap + size_idarray;
	      repo->lastoff = 0;
	      if (have_xdata)
		data.incoreoffset[(s - pool->solvables) - data.start] = data.incoredatalen;
	    }
	  nentries--;
	  dp = data_read_id_max(dp, &id, 0, numschemata, &data);
	  incore_add_id(&data, id);
	  if (keys[key].type == REPOKEY_TYPE_FIXARRAY)
	    {
	      if (!id)
		{
		  pool_debug(pool, SOLV_ERROR, "illegal fixarray\n");
		  data.error = SOLV_ERROR_CORRUPT;
		}
	      stack[keydepth - 1] = id;
	    }
	  keyp = schemadata + schemata[id];
	  break;
	default:
	  if (id == RPM_RPMDBID && s && (keys[key].type == REPOKEY_TYPE_U32 || keys[key].type == REPOKEY_TYPE_NUM))
	    {
	      if (keys[key].type == REPOKEY_TYPE_U32)
	        dp = data_read_u32(dp, (unsigned int *)&id);
	      else
	        dp = data_read_id_max(dp, &id, 0, 0, &data);
	      if (!repo->rpmdbid)
		repo->rpmdbid = repo_sidedata_create(repo, sizeof(Id));
	      repo->rpmdbid[(s - pool->solvables) - repo->start] = id;
	      break;
	    }
	  dps = dp;
	  dp = data_skip(dp, keys[key].type);
	  if (keys[key].storage == KEY_STORAGE_INCORE)
	    incore_add_blob(&data, dps, dp - dps);
	  break;
	}
    }
  /* should shrink idarraydata again */

  if (keydepth)
    {
      pool_debug(pool, SOLV_ERROR, "unexpected EOF, depth = %d\n", keydepth);
      data.error = SOLV_ERROR_CORRUPT;
    }
  if (!data.error)
    {
      if (dp > bufend)
        {
	  pool_debug(pool, SOLV_ERROR, "buffer overrun\n");
	  data.error = SOLV_ERROR_EOF;
        }
    }
  solv_free(buf);

  if (data.error)
    {
      /* free solvables */
      repo_free_solvable_block(repo, data.start, data.end - data.start, 1);
      /* free id array */
      repo->idarraysize -= size_idarray;
      /* free incore data */
      data.incoredata = solv_free(data.incoredata);
      data.incoredatalen = data.incoredatafree = 0;
    }

  if (data.incoredatafree)
    {
      /* shrink excess size */
      data.incoredata = solv_realloc(data.incoredata, data.incoredatalen);
      data.incoredatafree = 0;
    }

  for (i = 1; i < numkeys; i++)
    if (keys[i].storage == KEY_STORAGE_VERTICAL_OFFSET)
      break;
  if (i < numkeys && !data.error)
    {
      Id fileoffset = 0;
      unsigned int pagesize;
      
      /* we have vertical data, make it available */
      data.verticaloffset = solv_calloc(numkeys, sizeof(Id));
      for (i = 1; i < numkeys; i++)
        if (keys[i].storage == KEY_STORAGE_VERTICAL_OFFSET)
	  {
	    data.verticaloffset[i] = fileoffset;
	    fileoffset += keys[i].size;
	  }
      data.lastverticaloffset = fileoffset;
      pagesize = read_u32(&data);
      data.error = repopagestore_read_or_setup_pages(&data.store, data.fp, pagesize, fileoffset);
    }
  else
    {
      /* no longer needed */
      data.fp = 0;
    }
  solv_free(idmap);

  if (data.error)
    {
      /* XXX: free repodata? */
      return data.error;
    }

  if (parent)
    {
      /* overwrite stub repodata */
      repodata_freedata(parent);
      data.repodataid = parent->repodataid;
      *parent = data;
    }
  else
    {
      /* make it available as new repodata */
      if (!repo->nrepodata)
	{
	  repo->nrepodata = 1;
	  repo->repodata = solv_calloc(2, sizeof(data));
	}
      else
        repo->repodata = solv_realloc2(repo->repodata, repo->nrepodata + 1, sizeof(data));
      data.repodataid = repo->nrepodata;
      repo->repodata[repo->nrepodata++] = data;
    }

  /* create stub repodata entries for all external */
  if (!(flags & SOLV_ADD_NO_STUBS) && !parent)
    {
      for (key = 1 ; key < data.nkeys; key++)
	if (data.keys[key].name == REPOSITORY_EXTERNAL && data.keys[key].type == REPOKEY_TYPE_FLEXARRAY)
	  break;
      if (key < data.nkeys)
	repodata_create_stubs(repo->repodata + (repo->nrepodata - 1));
    }

  POOL_DEBUG(SOLV_DEBUG_STATS, "repo_add_solv took %d ms\n", solv_timems(now));
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo size: %d solvables\n", repo->nsolvables);
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo memory used: %d K incore, %d K idarray\n", data.incoredatalen/1024, repo->idarraysize / (int)(1024/sizeof(Id)));
  return 0;
}

