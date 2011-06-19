/* gridfs.c - A GridFS utility; example application
 * Copyright 2011 Gergely Nagy <algernon@balabit.hu>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <mongo.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

typedef struct
{
  gchar *host;
  gint port;
  gchar *db;
  gchar *coll;
  gchar *ns;
  gboolean verbose;
  gboolean slaveok;
  gboolean master_sync;
} config_t;

#define VLOG(...) { if (config->verbose) fprintf (stderr, __VA_ARGS__); }

gchar *
oid_to_string (const guint8* oid)
{
  static gchar os[25];
  gint j;

  for (j = 0; j < 12; j++)
    sprintf (&os[j * 2], "%02x", oid[j]);
  os[25] = 0;
  return os;
}

void
mongo_gridfs_error (mongo_sync_connection *conn, config_t *config, int e)
{
  gchar *error = NULL;

  if (!conn)
    {
      fprintf (stderr, "Error encountered: %s\n", strerror (errno));
      exit (1);
    }

  mongo_sync_cmd_get_last_error (conn, config->db, &error);
  fprintf (stderr, "Error encountered: %s\n", (error) ? error : strerror (e));
  g_free (error);
  exit (1);
}

mongo_sync_connection *
mongo_gridfs_connect (config_t *config)
{
  mongo_sync_connection *conn;

  VLOG ("Connecting to %s:%d/%s.%s...\n", config->host, config->port,
	config->db, config->coll);

  conn = mongo_sync_connect (config->host, config->port, config->slaveok);
  if (!conn)
    mongo_gridfs_error (conn, config, errno);

  if (config->master_sync)
    {
      VLOG ("Syncing to master...\n");
      conn = mongo_sync_reconnect (conn, TRUE);
      if (!conn)
	mongo_gridfs_error (conn, config, errno);
    }

  return conn;
}

void
mongo_gridfs_get (config_t *config, gint argc, gchar *argv[])
{
  mongo_sync_connection *conn;
  mongo_sync_gridfs *gfs;
  mongo_sync_gridfs_file *gfile;
  mongo_sync_cursor *cursor;
  gint64 n = 0;
  bson *query;
  int fd;

  gchar *gfn, *ofn;

  if (argc < 4)
    {
      fprintf (stderr, "Usage: %s get GRIDFS_FILENAME OUTPUT_FILENAME\n",
	       argv[0]);
      exit (1);
    }
  gfn = argv[2];
  ofn = argv[3];

  conn = mongo_gridfs_connect (config);
  gfs = mongo_sync_gridfs_new (conn, config->ns);
  if (!gfs)
    mongo_gridfs_error (conn, config, errno);

  VLOG ("Trying to find '%s'...\n", gfn);

  query = bson_build (BSON_TYPE_STRING, "filename", gfn, -1,
		      BSON_TYPE_NONE);
  bson_finish (query);
  gfile = mongo_sync_gridfs_find (gfs, query);
  if (!gfile)
    mongo_gridfs_error (conn, config, errno);
  bson_free (query);

  VLOG ("Opening output file '%s'...\n", ofn);
  fd = open (ofn, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd == -1)
    {
      fprintf (stderr, "Error opening output file '%s': %s\n",
	       ofn, strerror (errno));
      exit (1);
    }

  VLOG ("Writing '%s' -> '%s' (%" G_GINT64_FORMAT " bytes in %" G_GINT64_FORMAT
	" chunks)\n", gfn, ofn,
	mongo_sync_gridfs_file_get_length (gfile),
	mongo_sync_gridfs_file_get_chunks (gfile));

  cursor = mongo_sync_gridfs_file_cursor_new (gfile, 0, 0);
  if (!cursor)
    mongo_gridfs_error (conn, config, errno);

  while (mongo_sync_cursor_next (cursor))
    {
      gint32 size;
      guint8 *data;

      VLOG ("\rWriting chunk %" G_GINT64_FORMAT "...", n++);

      data = mongo_sync_gridfs_file_cursor_get_chunk (cursor, &size);
      if (!data)
	mongo_gridfs_error (conn, config, errno);

      write (fd, data, size);
      g_free (data);
    }
  mongo_sync_cursor_free (cursor);

  close (fd);

  mongo_sync_gridfs_free (gfs, TRUE);

  VLOG("\n");
}

void
mongo_gridfs_put (config_t *config, gint argc, gchar *argv[])
{
  mongo_sync_connection *conn;
  mongo_sync_gridfs *gfs;
  mongo_sync_gridfs_file *gfile;
  bson *meta;
  int fd;
  guint8 *data;
  struct stat st;

  gchar *gfn, *ifn;

  if (argc < 4)
    {
      fprintf (stderr, "Usage: %s put INPUT_FILENAME GRIDFS_FILENAME\n",
	       argv[0]);
      exit (1);
    }
  ifn = argv[2];
  gfn = argv[3];

  mongo_util_oid_init (0);

  conn = mongo_gridfs_connect (config);
  gfs = mongo_sync_gridfs_new (conn, config->ns);
  if (!gfs)
    mongo_gridfs_error (conn, config, errno);

  VLOG ("Opening input file: '%s'...\n", ifn);
  fd = open (ifn, O_RDONLY);
  if (!fd)
    {
      fprintf (stderr, "Error opening input file: %s\n",
	       strerror (errno));
      exit (1);
    }
  if (fstat (fd, &st) != 0)
    {
      fprintf (stderr, "Error stat'ing the input file: %s\n",
	       strerror (errno));
      exit (1);
    }

  data = mmap (NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (data == MAP_FAILED)
    {
      fprintf (stderr, "Error mmapping the input file: %s\n",
	       strerror (errno));
    }

  meta = bson_build (BSON_TYPE_STRING, "filename", gfn, -1,
		     BSON_TYPE_NONE);
  bson_finish (meta);

  VLOG ("Uploading '%s' -> '%s'...\n", ifn, gfn);

  gfile = mongo_sync_gridfs_file_new_from_buffer (gfs, meta, data, st.st_size);
  if (!gfile)
    mongo_gridfs_error (conn, config, errno);
  bson_free (meta);
  munmap (data, st.st_size);

  printf ("Uploaded file: %s (_id: %s; md5 = %s)\n", gfn,
	  oid_to_string (mongo_sync_gridfs_file_get_id (gfile)),
	  mongo_sync_gridfs_file_get_md5 (gfile));

  mongo_sync_gridfs_file_free (gfile);
  mongo_sync_gridfs_free (gfs, TRUE);
}

void
mongo_gridfs_list (config_t *config)
{
  mongo_sync_connection *conn;
  mongo_sync_cursor *cursor;
  bson *query;
  gchar *ns;

  conn = mongo_gridfs_connect (config);
  ns = g_strconcat (config->ns, ".files", NULL);

  query = bson_new ();
  bson_finish (query);
  cursor = mongo_sync_cursor_new (conn, ns,
				  mongo_sync_cmd_query (conn, ns, 0,
							0, 0, query, NULL));
  bson_free (query);

  while (mongo_sync_cursor_next (cursor))
    {
      bson *meta = mongo_sync_cursor_get_data (cursor);
      bson_cursor *c;
      const guint8 oid[12];
      gint32 i32, chunk_size;
      gint64 length, date;
      const gchar *md5, *filename = NULL;

      c = bson_find (meta, "_id");
      if (!bson_cursor_get_oid (c, (const guint8 **)&oid))
	mongo_gridfs_error (conn, config, errno);
      bson_cursor_free (c);

      c = bson_find (meta, "length");
      if (!bson_cursor_get_int32 (c, &i32))
	{
	  if (!bson_cursor_get_int64 (c, &length))
	    mongo_gridfs_error (conn, config, errno);
	}
      else
	length = i32;
      bson_cursor_free (c);

      c = bson_find (meta, "chunkSize");
      if (!bson_cursor_get_int32 (c, &chunk_size))
	mongo_gridfs_error (conn, config, errno);
      bson_cursor_free (c);

      c = bson_find (meta, "uploadDate");
      if (!bson_cursor_get_utc_datetime (c, &date))
	mongo_gridfs_error (conn, config, errno);
      bson_cursor_free (c);

      c = bson_find (meta, "md5");
      if (!bson_cursor_get_string (c, &md5))
	mongo_gridfs_error (conn, config, errno);
      bson_cursor_free (c);

      c = bson_find (meta, "filename");
      bson_cursor_get_string (c, &filename);
      bson_cursor_free (c);

      printf ("{ _id: ObjectID(\"%s\"), length: %" G_GINT64_FORMAT
	      ", chunkSize: %i, uploadDate: %"
	      G_GINT64_FORMAT ", md5: \"%s\"",

	      oid_to_string (oid), length, chunk_size, date, md5);

      if (filename)
	printf (", filename: \"%s\"", filename);
      printf (" }\n");

      if (config->verbose)
	{
	  c = bson_cursor_new (meta);
	  printf ("\tExtra metadata: [ ");
	  while (bson_cursor_next (c))
	    {
	      if (strcmp (bson_cursor_key (c), "_id") &&
		  strcmp (bson_cursor_key (c), "length") &&
		  strcmp (bson_cursor_key (c), "chunkSize") &&
		  strcmp (bson_cursor_key (c), "uploadDate") &&
		  strcmp (bson_cursor_key (c), "md5") &&
		  strcmp (bson_cursor_key (c), "filename"))
		{
		  printf ("%s (%s), ", bson_cursor_key (c),
			  bson_cursor_type_as_string (c));
		}
	    }
	  bson_cursor_free (c);
	  printf ("]\n");
	}
    }

  g_free (ns);
  mongo_sync_disconnect (conn);
}

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GOptionContext *context;
  config_t config = {
    NULL, 27017, NULL, NULL, NULL, FALSE, FALSE, FALSE
  };

  GOptionEntry entries[] =
    {
      { "host", 'h', 0, G_OPTION_ARG_STRING, &config.host,
	"Host to connect to", "HOST" },
      { "port", 'p', 0, G_OPTION_ARG_INT, &config.port, "Port", "PORT" },
      { "db", 'd', 0, G_OPTION_ARG_STRING, &config.db, "Database", "DB" },
      { "collection", 'c', 0, G_OPTION_ARG_STRING, &config.coll, "Collection",
	"COLL" },
      { "verbose", 'v', 0, G_OPTION_ARG_NONE, &config.verbose,
	"Be verbose", NULL },
      { "slave-ok", 's', 0, G_OPTION_ARG_NONE, &config.slaveok,
	"Connecting to slaves is ok", NULL },
      { "master-sync", 'm', 0, G_OPTION_ARG_NONE, &config.master_sync,
	"Reconnect to the replica master", NULL },
      { NULL, 0, 0, 0, NULL, NULL, NULL }
    };

  context = g_option_context_new ("- GridFS utility");
  g_option_context_add_main_entries (context, entries, "mongo-dump");
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_print ("option parsing failed: %s\n", error->message);
      exit (1);
    }

  if (!config.host || !config.port || !config.db ||
      !config.coll || argc < 2)
    {
      gchar *help = g_option_context_get_help (context, TRUE, NULL);

      printf ("%s", help);
      exit (1);
    }
  config.ns = g_strdup_printf ("%s.%s", config.db, config.coll);

  if (g_ascii_strcasecmp (argv[1], "get") == 0)
    mongo_gridfs_get (&config, argc, argv);
  else if (g_ascii_strcasecmp (argv[1], "put") == 0)
    mongo_gridfs_put (&config, argc, argv);
  else if (g_ascii_strcasecmp (argv[1], "list") == 0)
    mongo_gridfs_list (&config);

  g_free (config.ns);
  g_option_context_free (context);

  return 0;
}
