/*
  Axel -- A lighter download accelerator for Linux and other Unices

  Copyright 2001-2007 Wilmer van der Gaast
  Copyright 2008      Philipp Hagemeister
  Copyright 2016      Stephen Thirlwall
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

/* The .st file that lets an interrupted download be resumed.
 *
 * Version 2 binds the saved progress to the representation it came from:
 * next to the number of connections, the total of bytes done and the range
 * each connection was working on, it records the length of the remote file
 * and the validator the server gave for it -- a strong entity tag, or the
 * Last-Modified date -- under a format version and a magic.  A state file
 * that does not match what the server describes now is discarded, never
 * resumed from: the publisher may have replaced the file between runs, and
 * the old offsets would splice the two versions into one corrupt file.
 *
 * Anything that is not a complete v2 state file -- every older format
 * included -- cannot be tied to the representation either, and degrades
 * the same way: the download starts from scratch.
 *
 * The file is replaced atomically: new state is written to a temporary
 * name next to it and renamed over the old one, so a crash leaves either
 * the old state or the new, never half of one. */

#include "config.h"
#include "axel.h"
#include "assert.h"
#include "stfile.h"

#define STFILE_MAGIC "AXST"
#define STFILE_VERSION 2
/* Bound on the connection count a state file may claim, so a corrupt one
 * cannot make us allocate or read without end */
#define STFILE_MAX_CONNS 4096

static
char *
stfile_makename(const char *bname)
{
	const char suffix[] = ".st";
	const size_t bname_len = strlen(bname);
	char *buf = malloc(bname_len + sizeof(suffix));
	if (!buf) {
		perror("stfile_open");
		abort();
	}
	memcpy(buf, bname, bname_len);
	memcpy(buf + bname_len, suffix, sizeof(suffix));
	return buf;
}


int
stfile_unlink(const char *bname)
{
	char *stname = stfile_makename(bname);
	int ret = unlink(stname);
	free(stname);
	return ret;
}

int
stfile_access(const char *bname, int mode)
{
	char *stname = stfile_makename(bname);
	int ret = access(stname, mode);
	free(stname);
	return ret;
}


static
int
stfile_open(const char *bname, int flags, mode_t mode)
{
	char *stname = stfile_makename(bname);
	int fd = open(stname, flags, mode);
	free(stname);
	return fd;
}


static
bool
write_full(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		p += n;
		len -= n;
	}
	return true;
}

static
bool
read_full(int fd, void *buf, size_t len)
{
	char *p = buf;

	while (len) {
		ssize_t n = read(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (!n)
			return false;
		p += n;
		len -= n;
	}
	return true;
}


/* Does the state that was read describe the file the server just
 * described?  The length has to match, and over HTTP the validator too:
 * the entity tag exactly, or the Last-Modified date to the second.  FTP
 * has no validators; the length is all there is. */
static
bool
state_matches_server(const axel_t *axel, int64_t size, int64_t mtime,
		     const char *etag)
{
	if (axel->size == LLONG_MAX || size != axel->size)
		return false;

	if (PROTO_IS_FTP(axel->conn[0].proto) && !axel->conn[0].proxy)
		return true;

	if (*etag)
		return *axel->etag && !strcmp(etag, axel->etag);
	if (mtime)
		return (int64_t)axel->last_modified == mtime;

	/* Nothing the state could be tied to */
	return false;
}


/* Do the ranges just read tile the file the server just described?
 *
 * The chunks axel_divide() lays down tile the file in order, and a
 * connection stops at the end of its own chunk, so the offsets have to
 * climb from zero to exactly the size the server gave. */
static
bool
state_fits_download(const axel_t *axel)
{
	off_t start = 0;

	if (axel->bytes_done < 0 || axel->bytes_done > axel->size)
		return false;

	for (int i = 0; i < axel->conf->num_connections; i++) {
		const conn_t *conn = &axel->conn[i];

		if (conn->lastbyte > axel->size)
			return false;
		if (conn->currentbyte < start ||
		    conn->currentbyte > conn->lastbyte)
			return false;

		start = conn->lastbyte;
	}

	return start == axel->size;
}


int
stfile_load(axel_t *axel)
{
	int fd = stfile_open(axel->filename, O_RDONLY, 0);
	if (fd == -1)
		return 0;

	/* What to go back to if the state turns out not to be ours */
	uint16_t wanted_conns = axel->conf->num_connections;

	char magic[sizeof(STFILE_MAGIC) - 1];
	uint32_t version, nconns;
	int64_t size, bytes_done, mtime;
	uint16_t etag_len;
	char etag[MAX_STRING];

	bool ok = read_full(fd, magic, sizeof(magic))
	    && !memcmp(magic, STFILE_MAGIC, sizeof(magic))
	    && read_full(fd, &version, sizeof(version))
	    && version == STFILE_VERSION
	    && read_full(fd, &nconns, sizeof(nconns))
	    && nconns >= 1 && nconns <= STFILE_MAX_CONNS
	    && read_full(fd, &size, sizeof(size))
	    && read_full(fd, &bytes_done, sizeof(bytes_done))
	    && read_full(fd, &mtime, sizeof(mtime))
	    && read_full(fd, &etag_len, sizeof(etag_len))
	    && etag_len < sizeof(etag)
	    && read_full(fd, etag, etag_len);

	if (!ok) {
		/* Not a complete v2 state file: an older format, or a
		   corrupt or truncated one.  None of those can be tied to
		   what the server has now, so the only safe thing is to
		   start over. */
		axel_message(axel, _("State file %s.st cannot be verified "
				     "against the server, starting from "
				     "scratch."), axel->filename);
		close(fd);
		stfile_unlink(axel->filename);
		return 0;
	}
	etag[etag_len] = 0;

	if (!state_matches_server(axel, size, mtime, etag)) {
		axel_message(axel, _("The file on the server changed since "
				     "the download started, discarding the "
				     "old progress."));
		close(fd);
		stfile_unlink(axel->filename);
		return 0;
	}

	if (!axel_conn_resize(axel, nconns)) {
		close(fd);
		return -1;
	}

	axel->bytes_done = bytes_done;
	for (int i = 0; ok && i < axel->conf->num_connections; i++) {
		int64_t currentbyte, lastbyte;

		ok = read_full(fd, &currentbyte, sizeof(currentbyte))
		    && read_full(fd, &lastbyte, sizeof(lastbyte));
		axel->conn[i].currentbyte = currentbyte;
		axel->conn[i].lastbyte = lastbyte;
	}

	close(fd);

	if (!ok || !state_fits_download(axel)) {
		axel_message(axel, _("State file %s.st belongs to another "
				     "download, ignoring it."), axel->filename);
		axel->bytes_done = 0;
		stfile_unlink(axel->filename);
		return axel_conn_resize(axel, wanted_conns) ? 0 : -1;
	}

	axel_message(axel,
		     _("State file found: %jd bytes downloaded, %jd to go."),
		     (intmax_t)axel->bytes_done,
		     (intmax_t)(axel->size - axel->bytes_done));

	return 1;
}


/**
 * Save the state of the current download.
 */
void
stfile_save(axel_t *axel)
{
	/* No use for such a file if the server doesn't support
	   resuming anyway.. */
	if (!axel->conn[0].supported)
		return;

	/* Over HTTP the state is a promise to continue this exact
	   representation; with no validator to tie it to, it could never
	   be kept, so don't make it. */
	if ((!PROTO_IS_FTP(axel->conn[0].proto) || axel->conn[0].proxy) &&
	    !*axel->etag && !axel->last_modified)
		return;

	char *stname = stfile_makename(axel->filename);
	char *tmpname = malloc(strlen(stname) + sizeof(".tmp"));
	if (!tmpname) {
		free(stname);
		return;
	}
	strcpy(tmpname, stname);
	strcat(tmpname, ".tmp");

	int fd = open(tmpname, O_CREAT | O_TRUNC | O_WRONLY, 0666);
	if (fd == -1) {
		free(tmpname);
		free(stname);
		return;		/* Not 100% fatal.. */
	}

	const uint32_t version = STFILE_VERSION;
	const uint32_t nconns = axel->conf->num_connections;
	const int64_t size = axel->size;
	const int64_t bytes_done = axel->bytes_done;
	const int64_t mtime = axel->last_modified;
	const uint16_t etag_len = strlen(axel->etag);

	bool ok = write_full(fd, STFILE_MAGIC, sizeof(STFILE_MAGIC) - 1)
	    && write_full(fd, &version, sizeof(version))
	    && write_full(fd, &nconns, sizeof(nconns))
	    && write_full(fd, &size, sizeof(size))
	    && write_full(fd, &bytes_done, sizeof(bytes_done))
	    && write_full(fd, &mtime, sizeof(mtime))
	    && write_full(fd, &etag_len, sizeof(etag_len))
	    && write_full(fd, axel->etag, etag_len);

	for (int i = 0; ok && i < axel->conf->num_connections; i++) {
		const int64_t currentbyte = axel->conn[i].currentbyte;
		const int64_t lastbyte = axel->conn[i].lastbyte;

		ok = write_full(fd, &currentbyte, sizeof(currentbyte))
		    && write_full(fd, &lastbyte, sizeof(lastbyte));
	}

	if (close(fd))
		ok = false;

	/* Replace the old state atomically: a crash may leave the old
	   file or the new one, but never half of one */
	if (!ok || rename(tmpname, stname))
		unlink(tmpname);

	free(tmpname);
	free(stname);
}
