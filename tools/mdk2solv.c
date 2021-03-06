/*
 * Copyright (c) 2012, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * mdk2solv.c
 *
 * parse Mandriva/Mageie synthesis file
 *
 * reads from stdin
 * writes to stdout
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "pool.h"
#include "repo.h"
#include "repo_mdk.h"
#include "solv_xfopen.h"
#include "common_write.h"


static void
usage(int status)
{
  fprintf(stderr, "\nUsage:\n"
          "mdk2solv [-i <infoxml>]\n"
          "  reads a 'synthesis' repository from <stdin> and writes a .solv file to <stdout>\n"
          "  -i : info.xml file for extra attributes\n"
          "  -h : print help & exit\n"
         );
   exit(status);
}

int
main(int argc, char **argv)
{
  Pool *pool;
  Repo *repo;
  char *infofile;
  int c;

  while ((c = getopt(argc, argv, "i:")) >= 0)
    {
      switch(c)
	{
	case 'h':
	  usage(0);
	  break;
	case 'i':
	  infofile = optarg;
	  break;
	default:
	  usage(1);
	  break;
	}
    }
  pool = pool_create();
  repo = repo_create(pool, "<stdin>");
  repo_add_mdk(repo, stdin, REPO_NO_INTERNALIZE);
  if (infofile)
    {
      FILE *fp = solv_xfopen(infofile, "r");
      if (!fp)
	{
	  perror(infofile);
	  exit(1);
	}
      repo_add_mdk_info(repo, fp, REPO_EXTEND_SOLVABLES | REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE);
      fclose(fp);
    }
  repo_internalize(repo);
  tool_write(repo, 0, 0);
  pool_free(pool);
  exit(0);
}
