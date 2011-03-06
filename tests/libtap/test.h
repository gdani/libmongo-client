#ifndef LIBMONGO_CLIENT_TEST_H
#define LIBMONGO_CLIENT_TEST_H 1

#include "tap.h"
#include "bson.h"
#include "mongo-wire.h"
#include "mongo-sync.h"

typedef struct
{
  gchar *primary_host;
  gint primary_port;

  gchar *secondary_host;
  gint secondary_port;

  gchar *db;
  gchar *coll;
  gchar *ns;
} func_config_t;

extern func_config_t config;

#define begin_network_tests(n)						\
  do									\
    {									\
      struct sigaction sa;						\
      sa.sa_handler = SIG_IGN;						\
      sigemptyset(&sa.sa_mask);						\
      sa.sa_flags = 0;							\
      sigaction(SIGPIPE, &sa, NULL);					\
      skip(!test_env_setup (), n, "Environment not set up for network tests")

#define end_network_tests()			\
      test_env_free ();				\
    } while (0);				\
  endskip

#define _DOC_SIZE(doc,pos) GINT32_FROM_LE (*(gint32 *)(&doc[pos]))

#define RUN_TEST(n, t) \
  int		       \
  main (void)	       \
  {		       \
    plan (n);	       \
    test_##t ();       \
    return 0;	       \
  }

gboolean test_env_setup (void);
void test_env_free (void);

#define RUN_NET_TEST(n, t)						\
  int									\
  main (void)								\
  {									\
    if (!test_env_setup ())						\
      printf ("1..0 # skip, Environment not set up for network tests"); \
    else								\
      {									\
	plan (n);							\
	test_##t ();							\
	test_env_free ();						\
      }									\
    return 0;								\
  }

bson *test_bson_generate_full (void);
mongo_packet *test_mongo_wire_generate_reply (gboolean valid,
					      gint32 nreturn,
					      gboolean with_docs);
mongo_sync_connection *test_make_fake_sync_conn (gint fd,
						 gboolean slaveok);

#endif
