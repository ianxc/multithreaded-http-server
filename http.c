#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http.h"
#include "response.h"

#define GET_URI_FAILED -1
#define NOT_FOUND_REQUEST -2
#define MIME_MAP_LEN 4
#define REQ_PREFIX "GET /"
#define REQ_PREFIX_LEN 5
#define REQ_HTTP10 " HTTP/1.0\r\n"
#define REQ_HTTP11 " HTTP/1.1\r\n"
#define REQ_HTTP_LEN 11
#define CRLF "\r\n"
#define SP_CHAR ' '
#define PATH_ESCAPE "/../"
#define PATH_ESCAPE_TRAILING "/.."
#define PATH_ESCAPE_TRAILING_LEN 3
#define SLASH_CHAR '/'
#define DOT_CHAR '.'

// A HTTP Request parsing, processing, local file handling, and response construction library.

// Pre-computed mime map for simplicity and readibility.
const char *mime_map[MIME_MAP_LEN][2] = {
    {".html", "text/html"}, {".jpg", "image/jpeg"}, {".css", "text/css"}, {".js", "text/javascript"}};
const char mime_default[] = "application/octet-stream";

// Function prototypes
int get_request_uri(const request_t *req, char **uri_dest);
bool uri_has_escape(const char *uri, int uri_len);
const char *get_mime(const char *uri);
int get_path(const char *path_root, const char *uri, const int uri_len, char **path_dest);
int get_body_fd(const char *path);

// Process partial requests as they are updated on-the-fly, caching previous progress for improved performance.
enum request_stage_t process_partial_request(request_t *req, size_t buffer_len) {
    // Step 1: finding the GET method with the starting / in abs_path.
    if (!req->has_valid_method) {
        // We have not yet seen REQ_PREFIX.
        if (buffer_len >= REQ_PREFIX_LEN) {
            // REQ_PREFIX must be a prefix of the buffer.
            if (strncmp(req->buffer, REQ_PREFIX, REQ_PREFIX_LEN) == 0) {
                // Proceed to checking checking for HTTP-Version
                req->has_valid_method = true;
                req->slash_ptr = req->buffer + REQ_PREFIX_LEN - 1;
                req->last_ptr = req->buffer + REQ_PREFIX_LEN;
            } else {
                return BAD;
            }

        } else {
            // The buffer is too small - but it must be a prefix of REQ_PREFIX to continuing waiting for more data.
            if (strncmp(req->buffer, REQ_PREFIX, buffer_len) == 0) {
                return RECVING;
            } else {
                return BAD;
            }
        }
    }

    // Step 2: have a valid method, now look for a valid HTTP version with CRLF.
    // assert(req->has_valid_method);
    if (!req->has_valid_httpver) {
        // The buffer is updated, so look for the first space starting from where we began filling the buffer this recv
        // call. Since there one and only one space seen previously (in REQ_PREFIX), this would be the 2nd and last
        // space of the Request-Line.
        if (req->space_ptr == NULL) {
            req->space_ptr = strchr(req->last_ptr, SP_CHAR);
        }

        if (req->space_ptr == NULL) {
            // Still couldn't find a space, update next recv's starting position and recv() until we can find a space.
            req->last_ptr = req->buffer + buffer_len;
            return RECVING;
        }

        // Get the length from the space to the end of the string.
        size_t from_space_len = buffer_len - (req->space_ptr - req->buffer);
        if (from_space_len >= REQ_HTTP_LEN) {
            // Must have entire HTTP-Version followed by >= 1x CRLF already in the buffer.
            if (strncmp(req->space_ptr, REQ_HTTP10, REQ_HTTP_LEN) == 0 ||
                strncmp(req->space_ptr, REQ_HTTP11, REQ_HTTP_LEN) == 0) {
                req->has_valid_httpver = true;

            } else {
                return BAD;
            }

        } else {
            // Check for partial prefix of HTTP-Version
            if (strncmp(req->space_ptr, REQ_HTTP10, from_space_len) == 0 ||
                strncmp(req->space_ptr, REQ_HTTP11, from_space_len) == 0) {
                return RECVING;

            } else {
                return BAD;
            }
        }
    }
    // Search for <CRLF><CRLF> (a strrstr implementation would be average-case faster but this is fine too).
    // Ed #948 was my question: https://edstem.org/au/courses/7916/discussion/864451?answer=1948422
    // Going with [B], which requires 2x consecutive CRLF.
    // assert(req->has_valid_method && req->has_valid_httpver && req->space_ptr != NULL);
    char *end = strstr(req->space_ptr, CRLF CRLF);
    return end == NULL ? RECVING : VALID;
}

// Given a valid processes request object, extract and validate its URI for additional rules (path escape),
// open the file, get its mime, and build the response.
response_t *make_response(const char *path_root, const request_t *req) {
    if (path_root == NULL || req == NULL) {
        return NULL;
    }

    // Get URI from a well-formed request-line.
    // Allow for misformed headers to continue past this as long as the request-line is valid. Ed #887.
    char *uri = NULL;
    int uri_len = get_request_uri(req, &uri);
    if (uri_len == GET_URI_FAILED) {
        return response_create_400();
    }

    // 404 URIs which traverse upwards the directory tree.
    if (uri_has_escape(uri, uri_len)) {
        return response_create_404();
    }

    // get full path.
    char *body_path = NULL;
    int path_len = get_path(path_root, uri, uri_len, &body_path);
    if (path_len < 0) {
        // Prefer giving 404 over crashing or existing on malloc failure.
        return response_create_404();
    }

    // attempt to open the file.
    int body_fd = get_body_fd(body_path);
    free(body_path);
    body_path = NULL;
    if (body_fd < 0) {
        free(uri);
        uri = NULL;
        return response_create_404();
    }

    // get mime type.
    const char *mime = get_mime(uri);
    free(uri);
    uri = NULL;

    // craft response.
    response_t *res_ok = response_create_200(body_fd, mime);

    return res_ok;
}

// Gets the Request-Line URI given a processed request.
int get_request_uri(const request_t *req, char **uri_dest) {
    // Copy uri path to new array.
    int uri_len = req->space_ptr - req->slash_ptr;
    char *uri = malloc(sizeof(*uri) * (uri_len + 1));
    if (uri == NULL) {
        perror("malloc: get_request_uri");
        return GET_URI_FAILED;
    }
    strncpy(uri, req->slash_ptr, uri_len);
    uri[uri_len] = '\0';
    *uri_dest = uri;
    return uri_len;
}

// Takes two non-empty strings and returns true if str's suffix equals `suffix`.
bool strsuffix(const char *str, size_t str_len, const char *suffix, size_t suffix_len) {
    if (str == NULL || suffix == NULL || str_len <= 0 || suffix_len <= 0) {
        return false;
    }
    if (suffix_len > str_len) {
        return false;
    }
    return strncmp(str + str_len - suffix_len, suffix, suffix_len) == 0;
}

// True: contains escape, hence is a bad request. False: no escape, OK to continue handling.
bool uri_has_escape(const char *uri, int uri_len) {
    // Check for trailing "/.." escape.
    bool has_trailing_escape = strsuffix(uri, uri_len, PATH_ESCAPE_TRAILING, PATH_ESCAPE_TRAILING_LEN);
    if (has_trailing_escape) {
        return true;
    }

    // Check for /../ (No need to check for leading ../ since abs_path must begin with /).
    char *ret = strstr(uri, PATH_ESCAPE);
    bool has_middle_escape = ret != NULL;
    return has_middle_escape;
}

// Gets the mime-type string literal for a valid URI.
const char *get_mime(const char *uri) {
    // Guaranteed that uri contains at least one '/' since previous checks for abs_path have been done.
    char *last_slash = strrchr(uri, SLASH_CHAR);
    char *last_dot = strrchr(uri, DOT_CHAR);

    if (last_dot == NULL || last_slash > last_dot) {
        return mime_default;
    }

    // Linear search is very fast due to high locality and lookup-table compiler optimizations.
    for (int i = 0; i < MIME_MAP_LEN; i++) {
        if (strcmp(last_dot, mime_map[i][0]) == 0) {
            return mime_map[i][1];
        }
    }
    return mime_default;
}

// Gets the full path to a resource.
int get_path(const char *path_root, const char *uri, const int uri_len, char **path_dest) {
    // Allocate full path
    int path_len = strlen(path_root) + uri_len;
    char *full_path = malloc(sizeof(*full_path) * (path_len + 1));
    if (full_path == NULL) {
        // This reeeeally shouldn't happen, but drop the request if necessary
        perror("malloc: get_path");
        return NOT_FOUND_REQUEST;
    }
    // Concat string. TODO: check if \0 is needed at the end.
    strcpy(full_path, path_root);
    strcat(full_path, uri);
    *path_dest = full_path;
    return path_len;
}

// Open and return a handle to an existing file.
int get_body_fd(const char *path) {
    // Get file descriptor, if path points to a present filesystem location.
    int body_fd = open(path, O_RDONLY);
    if (body_fd < 0) {
        return NOT_FOUND_REQUEST;
    }

    // Check if regular file (as opposed to directory or FIFO).
    struct stat st;
    fstat(body_fd, &st);
    if (S_ISREG(st.st_mode)) {
        return body_fd;
    }
    // Not a regular file: clean up file descriptor and prepare 404.
    close(body_fd);
    return NOT_FOUND_REQUEST;
}