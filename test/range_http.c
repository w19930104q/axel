// SPDX-FileCopyrightText: Copyright 2026 Ismael Luceno <ismael@iodev.co.uk>
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * test/range_http.c -- Range-request validation and resume-state binding.
 *
 * Two contracts, exercised against a one-shot HTTP server this file forks
 * onto 127.0.0.1, and against state files in a temporary directory:
 *
 *   - A connection that asked for a range must accept only that range of
 *     that representation: a 200 to a ranged request, or a 206 whose
 *     Content-Range disagrees with the request, is what used to splice a
 *     replaced file into the middle of the old one.
 *   - The .st file records the length and the validator of the
 *     representation its progress belongs to; anything that cannot be
 *     tied to what the server has now -- a changed file, an old format --
 *     is discarded, not resumed from.
 */

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "axel.h"
#include "stfile.h"
#include "harness.h"

/* src/text.c owns these two; src/axel.c references them.  The stubs keep
 * the user interface out of the link. */
void
print_messages(axel_t *axel)
{
	message_t *m = axel->message;

	while (m) {
		message_t *next = m->next;

		free(m);
		m = next;
	}
	axel->message = axel->last_message = NULL;
}

char *
axel_size_human(char *dst, size_t len, size_t value)
{
	snprintf(dst, len, "%zu", value);
	return dst;
}

/* ------------------------------------------------------------------ */
/* A one-shot HTTP server: accepts one connection, reports the request */
/* over a pipe, answers with a scripted response, and exits.           */
/* ------------------------------------------------------------------ */

struct test_server {
	pid_t pid;
	int port;
	int req_fd;		/* read end: the request, then EOF */
};

static int
server_start(struct test_server *srv, const char *response)
{
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0)
		return -1;

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		.sin_port = 0,
	};
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(lfd, 1) < 0)
		return -1;

	socklen_t alen = sizeof(addr);
	if (getsockname(lfd, (struct sockaddr *)&addr, &alen) < 0)
		return -1;

	int reqpipe[2];
	if (pipe(reqpipe) < 0)
		return -1;

	pid_t pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		close(reqpipe[0]);
		int cfd = accept(lfd, NULL, NULL);
		if (cfd < 0)
			_exit(1);

		/* The request is a few header lines; read up to the blank
		 * line, then hand it over. */
		char req[8192];
		size_t len = 0;
		while (len < sizeof(req) - 1) {
			ssize_t n = read(cfd, req + len, 1);
			if (n <= 0)
				break;
			len += n;
			req[len] = 0;
			if (strstr(req, "\r\n\r\n"))
				break;
		}
		if (write(reqpipe[1], req, len) < 0)
			_exit(1);

		const char *p = response;
		size_t left = strlen(response);
		while (left) {
			ssize_t n = write(cfd, p, left);
			if (n <= 0)
				break;
			p += n;
			left -= n;
		}
		close(cfd);
		_exit(0);
	}

	close(reqpipe[1]);
	close(lfd);
	srv->pid = pid;
	srv->port = ntohs(addr.sin_port);
	srv->req_fd = reqpipe[0];
	return 0;
}

/* The request the server saw, NUL-terminated; "" when there was none. */
static size_t
server_request(struct test_server *srv, char *buf, size_t len)
{
	size_t got = 0;
	ssize_t n;

	while (got < len - 1 &&
	       (n = read(srv->req_fd, buf + got, len - 1 - got)) > 0)
		got += n;
	buf[got] = 0;
	return got;
}

static void
server_reap(struct test_server *srv)
{
	int status;

	close(srv->req_fd);
	waitpid(srv->pid, &status, 0);
}

/* ------------------------------------------------------------------ */
/* A conn_t pointed at the test server, set up the way axel sets up a  */
/* download connection.                                                */
/* ------------------------------------------------------------------ */

static void
test_conn(conn_t *conn, conf_t *conf, int port, off_t first, off_t last,
	  off_t size, const char *if_range)
{
	char url[128];

	memset(conn, 0, sizeof(*conn));
	memset(conf, 0, sizeof(*conf));
	conf->io_timeout = 5;

	conn->conf = conf;
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/file", port);
	conn_set(conn, url);
	conn->supported = true;
	conn->currentbyte = first;
	conn->lastbyte = last;
	conn->size = size;
	if (if_range)
		strlcpy(conn->http->if_range, if_range,
			sizeof(conn->http->if_range));
}

static const char resp_206_100_199[] =
	"HTTP/1.1 206 Partial Content\r\n"
	"Content-Range: bytes 100-199/1000\r\n"
	"Content-Length: 100\r\n"
	"\r\n";

static const char resp_200_full[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Length: 1000\r\n"
	"\r\n";

TEST(range_and_if_range_headers_are_sent)
{
	struct test_server srv;
	conf_t conf;
	conn_t conn;
	char req[8192];

	ASSERT_OK(server_start(&srv, resp_206_100_199));
	test_conn(&conn, &conf, srv.port, 100, 200, 1000, "\"v1\"");

	ASSERT(conn_setup(&conn));
	CHECK(strstr(conn.http->request->p, "Range: bytes=100-199\r\n"));
	CHECK(strstr(conn.http->request->p, "If-Range: \"v1\"\r\n"));

	ASSERT(conn_exec(&conn));
	CHECK_EQ(conn.range_invalid, 0);

	ASSERT(server_request(&srv, req, sizeof(req)) > 0);
	CHECK(strstr(req, "Range: bytes=100-199\r\n"));
	CHECK(strstr(req, "If-Range: \"v1\"\r\n"));

	conn_disconnect(&conn);
	server_reap(&srv);
}

TEST(open_ended_range_needs_no_if_range)
{
	static const char resp[] =
		"HTTP/1.1 206 Partial Content\r\n"
		"Content-Range: bytes 100-999/1000\r\n"
		"Content-Length: 900\r\n"
		"\r\n";
	struct test_server srv;
	conf_t conf;
	conn_t conn;
	char req[8192];

	ASSERT_OK(server_start(&srv, resp));
	test_conn(&conn, &conf, srv.port, 100, 0, 1000, NULL);

	ASSERT(conn_setup(&conn));
	CHECK(strstr(conn.http->request->p, "Range: bytes=100-\r\n"));
	CHECK(!strstr(conn.http->request->p, "If-Range:"));

	ASSERT(conn_exec(&conn));
	CHECK_EQ(conn.range_invalid, 0);

	ASSERT(server_request(&srv, req, sizeof(req)) > 0);
	CHECK(strstr(req, "Range: bytes=100-\r\n"));

	conn_disconnect(&conn);
	server_reap(&srv);
}

TEST(full_file_answer_to_a_range_request_is_refused)
{
	struct test_server srv;
	conf_t conf;
	conn_t conn;

	ASSERT_OK(server_start(&srv, resp_200_full));
	test_conn(&conn, &conf, srv.port, 100, 200, 1000, "\"v1\"");

	ASSERT(conn_setup(&conn));
	CHECK_EQ(conn_exec(&conn), 0);
	CHECK_EQ(conn.range_invalid, 1);

	conn_disconnect(&conn);
	server_reap(&srv);
}

TEST(content_range_with_wrong_start_is_refused)
{
	static const char resp[] =
		"HTTP/1.1 206 Partial Content\r\n"
		"Content-Range: bytes 0-99/1000\r\n"
		"Content-Length: 100\r\n"
		"\r\n";
	struct test_server srv;
	conf_t conf;
	conn_t conn;

	ASSERT_OK(server_start(&srv, resp));
	test_conn(&conn, &conf, srv.port, 100, 200, 1000, NULL);

	ASSERT(conn_setup(&conn));
	CHECK_EQ(conn_exec(&conn), 0);
	CHECK_EQ(conn.range_invalid, 1);

	conn_disconnect(&conn);
	server_reap(&srv);
}

TEST(content_range_with_wrong_end_is_refused)
{
	static const char resp[] =
		"HTTP/1.1 206 Partial Content\r\n"
		"Content-Range: bytes 100-200/1000\r\n"
		"Content-Length: 101\r\n"
		"\r\n";
	struct test_server srv;
	conf_t conf;
	conn_t conn;

	ASSERT_OK(server_start(&srv, resp));
	test_conn(&conn, &conf, srv.port, 100, 200, 1000, NULL);

	ASSERT(conn_setup(&conn));
	CHECK_EQ(conn_exec(&conn), 0);
	CHECK_EQ(conn.range_invalid, 1);

	conn_disconnect(&conn);
	server_reap(&srv);
}

TEST(content_range_with_wrong_total_is_refused)
{
	static const char resp[] =
		"HTTP/1.1 206 Partial Content\r\n"
		"Content-Range: bytes 100-199/2000\r\n"
		"Content-Length: 100\r\n"
		"\r\n";
	struct test_server srv;
	conf_t conf;
	conn_t conn;

	ASSERT_OK(server_start(&srv, resp));
	test_conn(&conn, &conf, srv.port, 100, 200, 1000, NULL);

	ASSERT(conn_setup(&conn));
	CHECK_EQ(conn_exec(&conn), 0);
	CHECK_EQ(conn.range_invalid, 1);

	conn_disconnect(&conn);
	server_reap(&srv);
}

TEST(partial_content_without_content_range_is_refused)
{
	static const char resp[] =
		"HTTP/1.1 206 Partial Content\r\n"
		"Content-Length: 100\r\n"
		"\r\n";
	struct test_server srv;
	conf_t conf;
	conn_t conn;

	ASSERT_OK(server_start(&srv, resp));
	test_conn(&conn, &conf, srv.port, 100, 200, 1000, NULL);

	ASSERT(conn_setup(&conn));
	CHECK_EQ(conn_exec(&conn), 0);
	CHECK_EQ(conn.range_invalid, 1);

	conn_disconnect(&conn);
	server_reap(&srv);
}

TEST(http_date_round_trip)
{
	char buf[64];

	/* 1994-11-06 08:49:37 UTC */
	CHECK_EQ(http_parse_date("Sun, 06 Nov 1994 08:49:37 GMT"),
		 784111777);
	http_format_date(buf, sizeof(buf), 784111777);
	CHECK_STR(buf, "Sun, 06 Nov 1994 08:49:37 GMT");

	CHECK_EQ(http_parse_date("not a date"), 0);
	CHECK_EQ(http_parse_date("Sun, 06 Foo 1994 08:49:37 GMT"), 0);
	CHECK_EQ(http_parse_date("Sun, 32 Nov 1994 08:49:37 GMT"), 0);
}

/* ------------------------------------------------------------------ */
/* The state file: what it records, and what it refuses to resume.     */
/* ------------------------------------------------------------------ */

static char st_dir[256];

static int
st_mkdir(void)
{
	strcpy(st_dir, "/tmp/axel-range-http-test-XXXXXX");
	return mkdtemp(st_dir) != NULL;
}

static void
st_rmdir(void)
{
	char path[MAX_STRING + 4];

	snprintf(path, sizeof(path), "%s/out.st", st_dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/out.st.tmp", st_dir);
	unlink(path);
	rmdir(st_dir);
}

/* An axel_t the way a run leaves it: two connections, 500 of 1000 bytes
 * done, the file known by its entity tag. */
static axel_t *
st_fixture(conf_t *conf)
{
	axel_t *axel = calloc(1, sizeof(*axel));

	conf->num_connections = 2;
	axel->conf = conf;
	axel->conn = calloc(2, sizeof(conn_t));
	for (int i = 0; i < 2; i++) {
		pthread_mutex_init(&axel->conn[i].lock, NULL);
		axel->conn[i].proto = PROTO_HTTP;
		axel->conn[i].supported = true;
	}
	snprintf(axel->filename, sizeof(axel->filename), "%s/out", st_dir);
	axel->size = 1000;
	strlcpy(axel->etag, "\"v1\"", sizeof(axel->etag));
	axel->bytes_done = 500;
	axel->conn[0].currentbyte = 250;
	axel->conn[0].lastbyte = 500;
	axel->conn[1].currentbyte = 750;
	axel->conn[1].lastbyte = 1000;
	return axel;
}

static void
st_fixture_free(axel_t *axel)
{
	print_messages(axel);
	free(axel->conn);
	free(axel);
}

static int
st_exists(const axel_t *axel)
{
	return stfile_access(axel->filename, F_OK) == 0;
}

TEST(state_file_round_trip)
{
	conf_t conf, conf2;

	ASSERT(st_mkdir());
	axel_t *saved = st_fixture(&conf);
	stfile_save(saved);
	CHECK(st_exists(saved));

	axel_t *loaded = st_fixture(&conf2);
	loaded->bytes_done = 0;
	loaded->conn[0].currentbyte = 0;
	loaded->conn[1].currentbyte = 0;

	ASSERT_EQ(stfile_load(loaded), 1);
	CHECK_EQ(loaded->bytes_done, 500);
	CHECK_EQ(loaded->conn[0].currentbyte, 250);
	CHECK_EQ(loaded->conn[0].lastbyte, 500);
	CHECK_EQ(loaded->conn[1].currentbyte, 750);
	CHECK_EQ(loaded->conn[1].lastbyte, 1000);

	st_fixture_free(saved);
	st_fixture_free(loaded);
	st_rmdir();
}

TEST(state_file_rejects_a_changed_entity_tag)
{
	conf_t conf, conf2;

	ASSERT(st_mkdir());
	axel_t *saved = st_fixture(&conf);
	stfile_save(saved);

	axel_t *loaded = st_fixture(&conf2);
	strlcpy(loaded->etag, "\"v2\"", sizeof(loaded->etag));

	CHECK_EQ(stfile_load(loaded), 0);
	CHECK(!st_exists(loaded));	/* discarded, not kept */

	st_fixture_free(saved);
	st_fixture_free(loaded);
	st_rmdir();
}

TEST(state_file_rejects_a_changed_size)
{
	conf_t conf, conf2;

	ASSERT(st_mkdir());
	axel_t *saved = st_fixture(&conf);
	stfile_save(saved);

	axel_t *loaded = st_fixture(&conf2);
	loaded->size = 2000;

	CHECK_EQ(stfile_load(loaded), 0);
	CHECK(!st_exists(loaded));

	st_fixture_free(saved);
	st_fixture_free(loaded);
	st_rmdir();
}

TEST(state_file_binds_to_last_modified_without_an_entity_tag)
{
	conf_t conf, conf2, conf3;

	ASSERT(st_mkdir());
	axel_t *saved = st_fixture(&conf);
	*saved->etag = 0;
	saved->last_modified = 784111777;
	stfile_save(saved);
	CHECK(st_exists(saved));

	axel_t *same = st_fixture(&conf2);
	*same->etag = 0;
	same->last_modified = 784111777;
	CHECK_EQ(stfile_load(same), 1);

	axel_t *changed = st_fixture(&conf3);
	*changed->etag = 0;
	changed->last_modified = 784111778;
	CHECK_EQ(stfile_load(changed), 0);

	st_fixture_free(saved);
	st_fixture_free(same);
	st_fixture_free(changed);
	st_rmdir();
}

TEST(state_file_is_not_written_without_a_validator)
{
	conf_t conf;

	ASSERT(st_mkdir());
	axel_t *axel = st_fixture(&conf);
	*axel->etag = 0;
	axel->last_modified = 0;
	stfile_save(axel);
	CHECK(!st_exists(axel));

	st_fixture_free(axel);
	st_rmdir();
}

TEST(legacy_state_file_is_discarded)
{
	conf_t conf;

	ASSERT(st_mkdir());
	axel_t *axel = st_fixture(&conf);

	/* The pre-v2 layout: a connection count, the bytes done, and one
	 * (current, last) pair per connection, nothing to tie it to the
	 * representation. */
	char path[MAX_STRING + 4];
	snprintf(path, sizeof(path), "%s.st", axel->filename);
	FILE *f = fopen(path, "w");
	ASSERT_NOTNULL(f);
	uint16_t nconns = 1;
	int64_t fields[3] = { 100, 0, 1000 };
	ASSERT_EQ(fwrite(&nconns, sizeof(nconns), 1, f), 1);
	ASSERT_EQ(fwrite(fields, sizeof(int64_t), 3, f), 3);
	fclose(f);

	CHECK_EQ(stfile_load(axel), 0);
	CHECK(!st_exists(axel));

	st_fixture_free(axel);
	st_rmdir();
}

TEST(truncated_state_file_is_discarded)
{
	conf_t conf;

	ASSERT(st_mkdir());
	axel_t *axel = st_fixture(&conf);

	char path[MAX_STRING + 4];
	snprintf(path, sizeof(path), "%s.st", axel->filename);
	FILE *f = fopen(path, "w");
	ASSERT_NOTNULL(f);
	ASSERT_EQ(fwrite("AXST", 1, 4, f), 4);	/* magic, then nothing */
	fclose(f);

	CHECK_EQ(stfile_load(axel), 0);
	CHECK(!st_exists(axel));

	st_fixture_free(axel);
	st_rmdir();
}

int
main(void)
{
	REGISTER_DESC(range_and_if_range_headers_are_sent,
		      "a resumed connection sends Range and If-Range");
	REGISTER_DESC(open_ended_range_needs_no_if_range,
		      "an open-ended range is sent without a validator");
	REGISTER_DESC(full_file_answer_to_a_range_request_is_refused,
		      "a 200 to a ranged request is refused, not written");
	REGISTER_DESC(content_range_with_wrong_start_is_refused,
		      "a 206 starting at the wrong offset is refused");
	REGISTER_DESC(content_range_with_wrong_end_is_refused,
		      "a 206 ending at the wrong offset is refused");
	REGISTER_DESC(content_range_with_wrong_total_is_refused,
		      "a 206 for another total length is refused");
	REGISTER_DESC(partial_content_without_content_range_is_refused,
		      "a 206 without Content-Range is refused");
	REGISTER_DESC(http_date_round_trip,
		      "an HTTP date parses and formats back to itself");
	REGISTER_DESC(state_file_round_trip,
		      "the state file saves and restores the progress");
	REGISTER_DESC(state_file_rejects_a_changed_entity_tag,
		      "a changed entity tag discards the old progress");
	REGISTER_DESC(state_file_rejects_a_changed_size,
		      "a changed length discards the old progress");
	REGISTER_DESC(state_file_binds_to_last_modified_without_an_entity_tag,
		      "Last-Modified alone binds the state to the file");
	REGISTER_DESC(state_file_is_not_written_without_a_validator,
		      "no state is saved when nothing can verify it");
	REGISTER_DESC(legacy_state_file_is_discarded,
		      "a pre-v2 state file is discarded, not resumed");
	REGISTER_DESC(truncated_state_file_is_discarded,
		      "a truncated state file is discarded, not resumed");

	RUN_ALL();
	return DONE();
}
