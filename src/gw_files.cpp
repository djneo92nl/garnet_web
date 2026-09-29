#include "gw_internal.h"

#if defined(GARNET_WEB_FILES)

#include <cJSON.h>

// File manager for the "Files" group: every filesystem the app registered
// with gwAddFs() (SD, SD_MMC, LittleFS, FFat...), all through the plain
// fs::FS API. Runs on the server task. That is safe against the app's own
// file use: IDF's FATFS and LittleFS VFS layers lock internally, and SPI SD
// transactions take the bus per call. Uploads and downloads stream in
// 4 KB chunks, so the file size never depends on free RAM.
//
//   GET  /api/fs                      filesystems + usage
//   GET  /api/fs/list?fs=&path=       directory listing
//   GET  /api/fs/get?fs=&path=[&dl=1] file contents (dl = as attachment)
//   POST /api/fs/put?fs=&path=        raw body -> file (via "<path>.part")
//   POST /api/fs/delete {fs,path}     file, or empty directory
//   POST /api/fs/mkdir  {fs,path}
//   POST /api/fs/rename {fs,from,to}  rename / move within one filesystem

namespace {

constexpr uint8_t kMaxFs = 4;
constexpr size_t kChunk = 4096;
constexpr size_t kMaxPath = 255;

struct FsEntry {
  const char *id;
  const char *title;
  fs::FS *fs;
  void *obj;
  uint64_t (*total)(void *);
  uint64_t (*used)(void *);
};

FsEntry filesystems[kMaxFs];
uint8_t fsCount = 0;

// ---- Request helpers --------------------------------------------------------------

esp_err_t reply(httpd_req_t *req, const char *status, cJSON *json) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char *text = json ? cJSON_PrintUnformatted(json) : nullptr;
  cJSON_Delete(json);
  esp_err_t err = httpd_resp_sendstr(req, text ? text : "{\"ok\":false}");
  cJSON_free(text);
  return err;
}

esp_err_t fail(httpd_req_t *req, const char *status, const char *error) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", false);
  cJSON_AddStringToObject(o, "error", error);
  return reply(req, status, o);
}

esp_err_t ok(httpd_req_t *req) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", true);
  return reply(req, "200 OK", o);
}

// httpd_query_key_value hands back the raw, still-%-encoded value.
String urlDecode(const char *s) {
  String out;
  for (; *s; s++) {
    if (*s == '+') {
      out += ' ';
    } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
      char hex[3] = {s[1], s[2], 0};
      out += (char)strtol(hex, nullptr, 16);
      s += 2;
    } else {
      out += *s;
    }
  }
  return out;
}

String queryParam(httpd_req_t *req, const char *key) {
  size_t len = httpd_req_get_url_query_len(req);
  if (len == 0 || len > 1024) return "";
  char *q = static_cast<char *>(malloc(len + 1));
  if (q == nullptr) return "";
  String value;
  if (httpd_req_get_url_query_str(req, q, len + 1) == ESP_OK) {
    char raw[kMaxPath * 3 + 1]; // worst case every byte %-encoded
    if (httpd_query_key_value(q, key, raw, sizeof(raw)) == ESP_OK) value = urlDecode(raw);
  }
  free(q);
  return value;
}

cJSON *readJson(httpd_req_t *req) {
  if (req->content_len == 0 || req->content_len > 2048) return nullptr;
  char *buf = static_cast<char *>(malloc(req->content_len + 1));
  if (buf == nullptr) return nullptr;
  size_t got = 0;
  while (got < req->content_len) {
    int r = httpd_req_recv(req, buf + got, req->content_len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) break;
    got += r;
  }
  buf[got] = '\0';
  cJSON *json = got == req->content_len ? cJSON_Parse(buf) : nullptr;
  free(buf);
  return json;
}

String jsonStr(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? String(v->valuestring) : String();
}

FsEntry *findFs(const String &id) {
  for (uint8_t i = 0; i < fsCount; i++) {
    if (id == filesystems[i].id) return &filesystems[i];
  }
  return nullptr;
}

// Absolute, no "..", no empty segments, no trailing slash (except "/").
// Rejecting ".." matters even though every path is FS-relative: VFS paths
// are joined onto the mount point, and "/../" would step out of it.
bool cleanPath(String &p) {
  if (p.length() == 0 || p.length() > kMaxPath || p[0] != '/') return false;
  while (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
  if (p.indexOf("//") >= 0) return false;
  int start = 1;
  while (start <= (int)p.length()) {
    int end = p.indexOf('/', start);
    if (end < 0) end = p.length();
    String seg = p.substring(start, end);
    if (seg == "." || seg == "..") return false;
    start = end + 1;
  }
  return true;
}

const char *mimeFor(const String &path) {
  String p = path;
  p.toLowerCase();
  struct Ext { const char *ext, *mime; };
  static const Ext kTypes[] = {
      {".htm", "text/html"},  {".html", "text/html"},       {".txt", "text/plain"},
      {".log", "text/plain"}, {".csv", "text/csv"},         {".json", "application/json"},
      {".css", "text/css"},   {".js", "text/javascript"},   {".jpg", "image/jpeg"},
      {".jpeg", "image/jpeg"}, {".png", "image/png"},        {".gif", "image/gif"},
      {".svg", "image/svg+xml"}, {".bmp", "image/bmp"},       {".pdf", "application/pdf"},
      {".wav", "audio/wav"},  {".mp3", "audio/mpeg"},
  };
  for (const Ext &t : kTypes) {
    if (p.endsWith(t.ext)) return t.mime;
  }
  return "application/octet-stream";
}

// ---- Handlers ----------------------------------------------------------------------

esp_err_t listFilesystems(httpd_req_t *req) {
  cJSON *arr = cJSON_CreateArray();
  for (uint8_t i = 0; i < fsCount; i++) {
    const FsEntry &e = filesystems[i];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", e.id);
    cJSON_AddStringToObject(o, "title", e.title);
    // Numbers as doubles: cJSON has no 64-bit ints, and a double is exact
    // up to 2^53 bytes - plenty for any SD card.
    cJSON_AddNumberToObject(o, "total", (double)e.total(e.obj));
    cJSON_AddNumberToObject(o, "used", (double)e.used(e.obj));
    cJSON_AddItemToArray(arr, o);
  }
  return reply(req, "200 OK", arr);
}

esp_err_t listDir(httpd_req_t *req, FsEntry &e, const String &path) {
  File dir = e.fs->open(path);
  if (!dir || !dir.isDirectory()) return fail(req, "404 Not Found", "No such folder");
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "path", path.c_str());
  cJSON *list = cJSON_AddArrayToObject(o, "entries");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    cJSON *ent = cJSON_CreateObject();
    cJSON_AddStringToObject(ent, "name", f.name());
    cJSON_AddBoolToObject(ent, "dir", f.isDirectory());
    if (!f.isDirectory()) cJSON_AddNumberToObject(ent, "size", (double)f.size());
    time_t t = f.getLastWrite();
    // FAT stores real dates; LittleFS may report 0 (no mtime support) and
    // a board without NTP writes 1970/1980 dates - leave those out.
    if (t > 946684800) cJSON_AddNumberToObject(ent, "time", (double)t);
    cJSON_AddItemToArray(list, ent);
    f.close();
  }
  dir.close();
  return reply(req, "200 OK", o);
}

esp_err_t getFile(httpd_req_t *req, FsEntry &e, const String &path, bool attachment) {
  File f = e.fs->open(path, FILE_READ);
  if (!f || f.isDirectory()) return fail(req, "404 Not Found", "No such file");
  httpd_resp_set_type(req, mimeFor(path));
  String name = path.substring(path.lastIndexOf('/') + 1);
  String disp = String(attachment ? "attachment" : "inline") + "; filename=\"" + name + "\"";
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  char *buf = static_cast<char *>(malloc(kChunk));
  if (buf == nullptr) {
    f.close();
    return fail(req, "500 Internal Server Error", "out of memory");
  }
  size_t n;
  esp_err_t err = ESP_OK;
  while ((n = f.read(reinterpret_cast<uint8_t *>(buf), kChunk)) > 0) {
    if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) break;
  }
  free(buf);
  f.close();
  return err == ESP_OK ? httpd_resp_send_chunk(req, nullptr, 0) : err;
}

esp_err_t putFile(httpd_req_t *req, FsEntry &e, const String &path) {
  if (path == "/") return fail(req, "422 Unprocessable Entity", "Missing file name");
  // Write beside the target, swap in only when complete: an upload cut off
  // halfway must not leave a truncated file under the real name (or eat
  // the existing one).
  String part = path + ".part";
  File f = e.fs->open(part, FILE_WRITE);
  if (!f) return fail(req, "500 Internal Server Error", "Cannot create file (folder missing or card full?)");
  char *buf = static_cast<char *>(malloc(kChunk));
  if (buf == nullptr) {
    f.close();
    e.fs->remove(part);
    return fail(req, "500 Internal Server Error", "out of memory");
  }
  size_t got = 0;
  bool good = true;
  while (got < req->content_len) {
    int r = httpd_req_recv(req, buf, min(kChunk, req->content_len - got));
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0 || f.write(reinterpret_cast<uint8_t *>(buf), r) != (size_t)r) {
      good = false;
      break;
    }
    got += r;
  }
  free(buf);
  f.close();
  if (!good) {
    e.fs->remove(part);
    return fail(req, "500 Internal Server Error", "Upload failed (card full or connection lost)");
  }
  if (e.fs->exists(path)) e.fs->remove(path);
  if (!e.fs->rename(part, path)) {
    e.fs->remove(part);
    return fail(req, "500 Internal Server Error", "Could not store file");
  }
  return ok(req);
}

esp_err_t jsonOp(httpd_req_t *req, const String &op) {
  cJSON *body = readJson(req);
  if (body == nullptr) return fail(req, "400 Bad Request", "invalid JSON");
  FsEntry *e = findFs(jsonStr(body, "fs"));
  String path = jsonStr(body, op == "rename" ? "from" : "path");
  String to = jsonStr(body, "to");
  cJSON_Delete(body);
  if (e == nullptr) return fail(req, "404 Not Found", "No such storage");
  if (!cleanPath(path) || path == "/") return fail(req, "422 Unprocessable Entity", "Invalid path");

  if (op == "mkdir") {
    if (e->fs->exists(path)) return fail(req, "422 Unprocessable Entity", "Already exists");
    return e->fs->mkdir(path) ? ok(req) : fail(req, "500 Internal Server Error", "Could not create folder");
  }
  if (op == "delete") {
    File f = e->fs->open(path);
    if (!f) return fail(req, "404 Not Found", "Not found");
    bool isDir = f.isDirectory();
    bool empty = true;
    if (isDir) {
      File child = f.openNextFile();
      empty = !child;
      child.close();
    }
    f.close();
    // Non-empty folders are refused rather than deleted recursively: one
    // mis-tap on a web page shouldn't be able to wipe a card.
    if (isDir && !empty) return fail(req, "422 Unprocessable Entity", "Folder is not empty");
    bool done = isDir ? e->fs->rmdir(path) : e->fs->remove(path);
    return done ? ok(req) : fail(req, "500 Internal Server Error", "Could not delete");
  }
  if (op == "rename") {
    if (!cleanPath(to) || to == "/") return fail(req, "422 Unprocessable Entity", "Invalid new path");
    if (e->fs->exists(to)) return fail(req, "422 Unprocessable Entity", "Target already exists");
    return e->fs->rename(path, to) ? ok(req) : fail(req, "500 Internal Server Error", "Could not rename (does the target folder exist?)");
  }
  return fail(req, "404 Not Found", "no such endpoint");
}

} // namespace

void gwAddFsRaw(const char *id, const char *title, fs::FS &fs, void *obj,
                uint64_t (*total)(void *), uint64_t (*used)(void *)) {
  if (fsCount >= kMaxFs) {
    log_e("garnet_web: too many filesystems (max %u)", (unsigned)kMaxFs);
    return;
  }
  filesystems[fsCount++] = {id, title, &fs, obj, total, used};
}

esp_err_t gwFilesHandle(httpd_req_t *req, const String &path, bool post) {
  if (!post && path == "/api/fs") return listFilesystems(req);
  if (post && path.startsWith("/api/fs/") && path != "/api/fs/put") {
    return jsonOp(req, path.substring(strlen("/api/fs/")));
  }

  FsEntry *e = findFs(queryParam(req, "fs"));
  if (e == nullptr) return fail(req, "404 Not Found", "No such storage");
  String p = queryParam(req, "path");
  if (!cleanPath(p)) return fail(req, "422 Unprocessable Entity", "Invalid path");

  if (!post && path == "/api/fs/list") return listDir(req, *e, p);
  if (!post && path == "/api/fs/get") return getFile(req, *e, p, queryParam(req, "dl") == "1");
  if (post && path == "/api/fs/put") return putFile(req, *e, p);
  return fail(req, "404 Not Found", "no such endpoint");
}

bool gwFilesEnabled() { return fsCount > 0; }

#endif // GARNET_WEB_FILES
