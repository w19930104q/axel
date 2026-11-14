/*
  Axel -- A lighter download accelerator for Linux and other Unices

  Copyright 2001-2007 Wilmer van der Gaast
  Copyright 2017-2026 Ismael Luceno

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  In addition, as a special exception, the copyright holders give
  permission to link the code of portions of this program with the
  OpenSSL library under certain conditions as described in each
  individual source file, and distribute linked combinations including
  the two.

  You must obey the GNU General Public License in all respects for all
  of the code used other than OpenSSL. If you modify file(s) with this
  exception, you may extend this exception to your version of the
  file(s), but you are not obligated to do so. If you do not wish to do
  so, delete this exception statement from your version. If you delete
  this exception statement from all source files in the program, then
  also delete it here.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Keeping the download tied to the remote representation.
 *
 * A download is only ever continued, across connections and across runs,
 * while the server keeps describing the same file: the validators the
 * server gave are remembered here, sent back as If-Range on every ranged
 * request, and recorded in the state file.  When the server stops
 * honouring that tie -- it answers a range with a 200, or its Content-Range
 * disagrees -- the partial data can no longer be trusted to belong to what
 * the server has now, and the only way out is to probe the server again
 * and start the whole download over. */

#include "config.h"
#include "axel.h"
#include "stfile.h"

/* How often a download may be started over because the server changed the
 * file or refused a range, before it is called hopeless */
#define MAX_SCRATCH_RESTARTS 3


/* Remember what the server said about the representation, so this run and
 * a later one can tell whether the file is still the same.  Only a strong
 * entity tag is kept: a weak one cannot anchor a range request. */
void
axel_save_validators(axel_t *axel)
{
	const char *e = http_header(axel->conn[0].http, "etag:");

	*axel->etag = 0;
	if (e) {
		while (*e == ' ' || *e == '\t')
			e++;
		if (e[0] != 'W' || e[1] != '/') {
			size_t len = strcspn(e, "\n");
			while (len && (e[len - 1] == ' ' || e[len - 1] == '\t'))
				len--;
			len = min(len, sizeof(axel->etag) - 1);
			memcpy(axel->etag, e, len);
			axel->etag[len] = 0;
		}
	}

	const char *m = http_header(axel->conn[0].http, "last-modified:");
	axel->last_modified = m ? http_parse_date(m) : 0;
}

/* The value to send as If-Range: the entity tag when there is one, the
 * Last-Modified date otherwise, empty when the server gave neither. */
static
void
axel_if_range_value(const axel_t *axel, char *dst, size_t len)
{
	if (*axel->etag)
		strlcpy(dst, axel->etag, len);
	else if (axel->last_modified)
		http_format_date(dst, len, axel->last_modified);
	else
		*dst = 0;
}

/* Hand every connection the size and the validator of the representation
 * it is about to ask ranges of. */
void
axel_set_conn_validators(axel_t *axel)
{
	char if_range[MAX_STRING];

	axel_if_range_value(axel, if_range, sizeof(if_range));
	for (int i = 0; i < axel->conf->num_connections; i++) {
		axel->conn[i].size = axel->size;
		axel->conn[i].range_invalid = false;
		strlcpy(axel->conn[i].http->if_range, if_range,
			sizeof(axel->conn[i].http->if_range));
	}
}

/* The server refused a range, or the file behind the URL changed under
 * the download: the partial data can no longer be trusted to belong to
 * what the server has now.  Probe the server again, throw the old progress
 * away, and start the whole download over.  This is the only way out --
 * resuming at the old offsets would splice two representations together.
 */
void
axel_restart_scratch(axel_t *axel)
{
	if (++axel->restarts > MAX_SCRATCH_RESTARTS) {
		axel_message(axel, _("The file on the server keeps changing, "
				     "giving up."));
		axel->ready = -1;
		return;
	}
	axel_message(axel, _("The file on the server changed or it refused "
			     "a range request; restarting from scratch."));

	for (int i = 0; i < axel->conf->num_connections; i++) {
		if (*axel->conn[i].setup_thread) {
			pthread_cancel(*axel->conn[i].setup_thread);
			join_setup_thread(&axel->conn[i]);
			/* A cancelled thread may have died holding its lock */
			pthread_mutex_destroy(&axel->conn[i].lock);
			pthread_mutex_init(&axel->conn[i].lock, NULL);
		}
		conn_disconnect(&axel->conn[i]);
		axel->conn[i].state = false;
	}

	/* Find out what the server has now */
	if (!conn_info(&axel->conn[0])) {
		char msg[80];
		int code = conn_info_status_get(msg, sizeof(msg), axel->conn);
		axel_message(axel, _("ERROR %d: %s."), code, msg);
		axel->ready = -1;
		return;
	}
	conn_url(axel->url->text, sizeof(axel->url->text) - 1, axel->conn);
	axel->size = axel->conn[0].size;
	axel_save_validators(axel);

	if (!axel->conn[0].supported && axel->conf->num_connections > 1)
		axel_conn_resize(axel, 1);

	axel->bytes_done = 0;
	axel->start_byte = 0;
	axel->start_time = axel_gettime();
	axel_divide(axel);

	/* The old progress described another representation */
	stfile_unlink(axel->filename);
	(void)ftruncate(axel->outfd, 0);
	if (axel->size != LLONG_MAX)
		(void)ftruncate(axel->outfd, axel->size);

	axel_set_conn_validators(axel);
	/* The next restart_connections() pass picks the connections up. */
}
