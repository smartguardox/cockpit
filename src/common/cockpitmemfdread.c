/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include "cockpitmemfdread.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static gchar *
cockpit_memfd_read (int      fd,
                    GError **error)
{
  int seals = fcntl (fd, F_GET_SEALS);
  if (seals == -1)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "could not query seals on fd %d: not memfd?: %m", fd);
      return NULL;
    }

  const guint expected_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
  if ((seals & expected_seals) != expected_seals)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "memfd fd %d has incorrect seals set: %u (instead of %u)\n",
                   fd, seals & expected_seals, expected_seals);
      return NULL;
    }

  struct stat buf;
  if (fstat (fd, &buf) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Failed to stat memfd %d: %m", fd);
      return NULL;
    }

  if (buf.st_size < 1)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "memfd %d must not be empty", fd);
      return NULL;
    }

  g_autofree gchar *content = g_malloc (buf.st_size + 1);
  gssize s = pread (fd, content, buf.st_size + 1, 0);
  if (s == -1)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "failed to read memfd %d: %m", fd);
      return NULL;
    }
  else if (s != buf.st_size)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "memfd %d changed size from %zu to %zu bytes", fd, (gssize) buf.st_size, s);
      return NULL;
    }

  content[buf.st_size] = '\0';

  return g_steal_pointer (&content);
}

gboolean
cockpit_memfd_read_from_envvar (gchar      **result,
                                const char  *envvar,
                                GError     **error)
{
  const gchar *fd_str = g_getenv (envvar);

  if (fd_str == NULL)
    {
      /* Environment variable unset is a valid (empty) result. */
      *result = NULL;
      return TRUE;
    }

  char *end;
  long value = strtol (fd_str, &end, 10);
  if (*end || value < 0 || value >= INT_MAX)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "invalid value for %s environment variable: %s", envvar, fd_str);
      return FALSE;
    }
  int fd = (int) value;
  g_unsetenv (envvar);

  gchar *content = cockpit_memfd_read (fd, error);
  close (fd);

  if (content == NULL)
    return FALSE;

  *result = content;
  return TRUE;
}
