/* Covers the shared code where a quiet bug would corrupt installs or let a
 * hostile game id escape the library directory. Run via `ctest`. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "vapor/buf.h"
#include "vapor/iso9660.h"
#include "vapor/manifest.h"
#include "vapor/sha256.h"
#include "vapor/util.h"
#include "vapor/wise.h"

#include <stdint.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures;

static void
check(int cond, const char *what)
{
    if (!cond) {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

static void
check_hex(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want) != 0) {
        printf("  FAIL  %s\n        got  %s\n        want %s\n", what, got, want);
        failures++;
    }
}

static void
test_sha256(void)
{
    char  hex[VAPOR_SHA256_HEX_LEN + 1];
    char *big;

    puts("sha256");

    vapor_sha256_hex_buf("", 0, hex);
    check_hex(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b785"
                   "2b855", "empty string");

    vapor_sha256_hex_buf("abc", 3, hex);
    check_hex(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20"
                   "015ad", "\"abc\"");

    /* 56 bytes: forces the padding to spill into a second block. */
    vapor_sha256_hex_buf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopn"
                         "opq", 56, hex);
    check_hex(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419d"
                   "b06c1", "56-byte input");

    /* Exactly one block: no published vector needed, but a one-shot hash and a
     * split-across-the-boundary hash of the same bytes must agree. */
    {
        static const char *b64 = "abcdefghbcdefghicdefghijdefghijkefghijklfghij"
                                 "klmghijklmnhijklmno";
        char          split[VAPOR_SHA256_HEX_LEN + 1];
        vapor_sha256  c;
        uint8_t       d[VAPOR_SHA256_DIGEST_LEN];

        vapor_sha256_hex_buf(b64, 64, hex);
        vapor_sha256_init(&c);
        vapor_sha256_update(&c, b64, 32);
        vapor_sha256_update(&c, b64 + 32, 32);
        vapor_sha256_final(&c, d);
        vapor_sha256_hex(d, split);
        check_hex(split, hex, "64-byte input, split at the block boundary");
    }

    /* 1,000,000 'a' exercises the 64-bit length field and streaming updates. */
    big = (char *)malloc(1000000);
    check(big != NULL, "allocate 1MB");
    if (big) {
        vapor_sha256 c;
        int          i;
        memset(big, 'a', 1000000);
        vapor_sha256_hex_buf(big, 1000000, hex);
        check_hex(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39cc"
                       "c7112cd0", "one million 'a' (one shot)");

        /* Same input fed in awkward chunk sizes must agree. */
        vapor_sha256_init(&c);
        for (i = 0; i < 1000000; i += 7) {
            size_t n = (size_t)(1000000 - i) < 7 ? (size_t)(1000000 - i) : 7;
            vapor_sha256_update(&c, big + i, n);
        }
        {
            uint8_t d[VAPOR_SHA256_DIGEST_LEN];
            vapor_sha256_final(&c, d);
            vapor_sha256_hex(d, hex);
        }
        check_hex(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39cc"
                       "c7112cd0", "one million 'a' (7-byte chunks)");
        free(big);
    }
}

static void
test_version_cmp(void)
{
    puts("version_cmp");
    check(vapor_version_cmp("1.0", "1.0") == 0, "equal");
    check(vapor_version_cmp("1.10", "1.9") > 0, "1.10 > 1.9 (numeric, not lexical)");
    check(vapor_version_cmp("1.9", "1.10") < 0, "1.9 < 1.10");
    check(vapor_version_cmp("1.2", "1.2.1") < 0, "prefix sorts first");
    check(vapor_version_cmp("1.2.1", "1.2") > 0, "longer sorts later");
    check(vapor_version_cmp("2.0", "10.0") < 0, "2.0 < 10.0");
    check(vapor_version_cmp("1.0.3", "1.0.3") == 0, "three-part equal");
    check(vapor_version_cmp("", "") == 0, "empty equal");
}

static void
test_id_validation(void)
{
    puts("id_is_valid");
    check(vapor_id_is_valid("hollow-vale"), "hyphens allowed");
    check(vapor_id_is_valid("game_2"), "underscore and digit");
    check(vapor_id_is_valid("a"), "single char");
    check(!vapor_id_is_valid(""), "empty rejected");
    check(!vapor_id_is_valid(NULL), "NULL rejected");
    check(!vapor_id_is_valid("../etc/passwd"), "traversal rejected");
    check(!vapor_id_is_valid("a..b"), "embedded .. rejected");
    check(!vapor_id_is_valid(".hidden"), "leading dot rejected");
    check(!vapor_id_is_valid("-flag"), "leading dash rejected");
    check(!vapor_id_is_valid("Hollow"), "uppercase rejected");
    check(!vapor_id_is_valid("with space"), "space rejected");
    check(!vapor_id_is_valid("semi;colon"), "shell metachar rejected");
    check(!vapor_id_is_valid("back\\slash"), "backslash rejected");
    check(!vapor_id_is_valid("for/slash"), "slash rejected");
}

static void
test_id_slug(void)
{
    char id[VAPOR_ID_MAX + 1];

    puts("id_slug");
    check(vapor_id_slug("Hollow Knight", id, sizeof(id)) == 0
              && strcmp(id, "hollow-knight") == 0,
          "spaces to dashes");
    check(vapor_id_slug("Call of Duty: MW2", id, sizeof(id)) == 0
              && strcmp(id, "call-of-duty-mw2") == 0,
          "punctuation collapsed");
    check(vapor_id_slug("Game", id, sizeof(id)) == 0 && strcmp(id, "game") == 0,
          "lowercase");
    check(vapor_id_slug("  --  ", id, sizeof(id)) == 0 && strcmp(id, "game") == 0,
          "empty after strip becomes game");
    check(vapor_id_slug("123", id, sizeof(id)) == 0 && strcmp(id, "123") == 0,
          "digits kept");
    check(vapor_id_is_valid(id), "slug is a valid id");
}

static void
test_username_validation(void)
{
    puts("username_is_valid");
    check(vapor_username_is_valid("ada"), "short name");
    check(vapor_username_is_valid("Ada_Lovelace"), "mixed case and underscore");
    check(vapor_username_is_valid("user-1.2"), "dash and dot");
    check(!vapor_username_is_valid(""), "empty rejected");
    check(!vapor_username_is_valid(NULL), "NULL rejected");
    check(!vapor_username_is_valid("ab"), "too short");
    check(!vapor_username_is_valid("has space"), "space rejected");
    check(!vapor_username_is_valid("bad@name"), "at-sign rejected");
}

static void
test_glob(void)
{
    puts("glob_match");
    check(vapor_glob_match("lib/*.so*", "lib/libfoo.so.1"), "versioned so");
    check(vapor_glob_match("lib/*.so*", "lib/libfoo.so"), "plain so");
    check(vapor_glob_match("bin/game", "bin/game"), "literal");
    check(vapor_glob_match("*", "anything/at/all"), "star spans slashes");
    check(vapor_glob_match("bin/?ame", "bin/game"), "question mark");
    check(!vapor_glob_match("*.exe", "game.dll"), "extension mismatch");
    check(!vapor_glob_match("bin/game", "bin/game2"), "trailing chars");
    check(vapor_glob_match("", ""), "empty matches empty");
    check(!vapor_glob_match("", "x"), "empty pattern vs text");
}

static void
test_buf(void)
{
    vapor_buf b;

    puts("buf");
    vapor_buf_init(&b);
    check(vapor_buf_appends(&b, "hello") == 0, "appends");
    check(vapor_buf_appendf(&b, " %s %d", "world", 42) == 0, "appendf");
    check(b.len == strlen("hello world 42"), "length tracks");
    check(strcmp(b.data, "hello world 42") == 0, "contents");
    vapor_buf_reset(&b);
    check(b.len == 0 && b.data[0] == '\0', "reset");
    vapor_buf_free(&b);
    check(b.data == NULL, "free clears");
}

static void
test_manifest_roundtrip(void)
{
    static const char *json =
        "{\"schema\":1,\"id\":\"hollow-vale\",\"name\":\"Hollow Vale\","
        "\"version\":\"1.0.3\",\"developer\":\"Someone\","
        "\"description\":\"A test.\",\"cover\":\"cover.png\","
        "\"package\":{\"file\":\"package.zip\",\"format\":\"zip\","
        "\"size\":1234,"
        "\"sha256\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b"
        "7852b855\",\"strip_prefix\":\"HollowVale/\"},"
        "\"targets\":["
        "{\"platform\":\"windows\",\"arch\":\"x86_64\","
        "\"exec\":\"bin/HollowVale.exe\",\"cwd\":\"bin\"},"
        "{\"platform\":\"linux\",\"arch\":\"x86_64\",\"exec\":\"bin/hollowvale\","
        "\"args\":[\"--fullscreen\"],\"exec_bits\":[\"bin/hollowvale\"],"
        "\"env\":{\"LD_LIBRARY_PATH\":\"$INSTALL_DIR/lib\"}}"
        "]}";
    vapor_manifest m, m2;
    char           err[256];
    char          *text;
    const vapor_target *t;

    puts("manifest");
    check(vapor_manifest_parse(json, strlen(json), &m, err, sizeof(err)) == 0,
          "parses valid manifest");
    check(m.schema == 1, "schema");
    check(m.id && strcmp(m.id, "hollow-vale") == 0, "id");
    check(m.cover && strcmp(m.cover, "cover.png") == 0, "cover filename");
    check(m.package.size == 1234, "package size");
    check(m.ntargets == 2, "two targets");
    check(m.package.strip_prefix
          && strcmp(m.package.strip_prefix, "HollowVale/") == 0, "strip_prefix");

    t = vapor_manifest_pick_target(&m, "linux", "x86_64");
    check(t != NULL, "picks linux target");
    if (t) {
        check(t->nargs == 1 && strcmp(t->args[0], "--fullscreen") == 0, "args");
        check(t->nenv == 1 && strcmp(t->env[0].key, "LD_LIBRARY_PATH") == 0, "env");
        check(t->nexec_bits == 1, "exec_bits");
    }
    check(vapor_manifest_pick_target(&m, "macos", "x86_64") == NULL,
          "no target for unknown platform");

    /* Serialize and reparse: the round trip must preserve the fields the
     * installer and launcher depend on. */
    text = vapor_manifest_serialize(&m);
    check(text != NULL, "serializes");
    if (text) {
        check(vapor_manifest_parse(text, strlen(text), &m2, err, sizeof(err)) == 0,
              "reparses its own output");
        check(m2.ntargets == m.ntargets, "target count survives");
        check(strcmp(m2.package.sha256, m.package.sha256) == 0, "sha survives");
        check(m2.package.size == m.package.size, "size survives");
        check(m2.id && strcmp(m2.id, m.id) == 0, "id survives");
        check(m2.cover && strcmp(m2.cover, "cover.png") == 0, "cover survives");
        vapor_manifest_free(&m2);
        free(text);
    }
    vapor_manifest_free(&m);

    /* Rejections that matter for safety. */
    {
        static const char *bad_id =
            "{\"schema\":1,\"id\":\"../escape\",\"name\":\"x\",\"version\":\"1\","
            "\"package\":{\"file\":\"p.zip\",\"size\":1,\"sha256\":\"e3b0c44298fc"
            "1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"},"
            "\"targets\":[{\"platform\":\"linux\",\"exec\":\"a\"}]}";
        static const char *bad_exec =
            "{\"schema\":1,\"id\":\"ok\",\"name\":\"x\",\"version\":\"1\","
            "\"package\":{\"file\":\"p.zip\",\"size\":1,\"sha256\":\"e3b0c44298fc"
            "1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"},"
            "\"targets\":[{\"platform\":\"linux\",\"exec\":\"../../bin/sh\"}]}";
        static const char *bad_schema =
            "{\"schema\":99,\"id\":\"ok\",\"name\":\"x\",\"version\":\"1\","
            "\"package\":{\"file\":\"p.zip\",\"size\":1,\"sha256\":\"e3b0c44298fc"
            "1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"},"
            "\"targets\":[{\"platform\":\"linux\",\"exec\":\"a\"}]}";
        static const char *short_sha =
            "{\"schema\":1,\"id\":\"ok\",\"name\":\"x\",\"version\":\"1\","
            "\"package\":{\"file\":\"p.zip\",\"size\":1,\"sha256\":\"abcd\"},"
            "\"targets\":[{\"platform\":\"linux\",\"exec\":\"a\"}]}";
        static const char *bad_cover =
            "{\"schema\":1,\"id\":\"ok\",\"name\":\"x\",\"version\":\"1\","
            "\"cover\":\"../secret.png\","
            "\"package\":{\"file\":\"p.zip\",\"size\":1,\"sha256\":\"e3b0c44298fc"
            "1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"},"
            "\"targets\":[{\"platform\":\"linux\",\"exec\":\"a\"}]}";

        check(vapor_manifest_parse(bad_id, strlen(bad_id), &m, err, sizeof(err)) != 0,
              "rejects traversal in id");
        check(vapor_manifest_parse(bad_exec, strlen(bad_exec), &m, err, sizeof(err)) != 0,
              "rejects traversal in exec");
        check(vapor_manifest_parse(bad_schema, strlen(bad_schema), &m, err, sizeof(err)) != 0,
              "rejects unknown schema");
        check(vapor_manifest_parse(short_sha, strlen(short_sha), &m, err, sizeof(err)) != 0,
              "rejects malformed sha256");
        check(vapor_manifest_parse("not json", 8, &m, err, sizeof(err)) != 0,
              "rejects non-JSON");
        check(vapor_manifest_parse(bad_cover, strlen(bad_cover), &m, err,
                                   sizeof(err))
                  != 0,
              "rejects traversal in cover");
        {
            static const char *iso_empty =
                "{\"schema\":1,\"id\":\"ok\",\"name\":\"x\",\"version\":\"1\","
                "\"package\":{\"file\":\"disc.iso\",\"format\":\"iso\",\"size\":1,"
                "\"sha256\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca4"
                "95991b7852b855\"},\"targets\":[]}";
            static const char *zip_empty =
                "{\"schema\":1,\"id\":\"ok\",\"name\":\"x\",\"version\":\"1\","
                "\"package\":{\"file\":\"package.zip\",\"format\":\"zip\",\"size\":1,"
                "\"sha256\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca4"
                "95991b7852b855\"},\"targets\":[]}";
            if (vapor_manifest_parse(iso_empty, strlen(iso_empty), &m, err,
                                     sizeof(err))
                == 0) {
                check(m.ntargets == 0, "iso may have no targets");
                vapor_manifest_free(&m);
            } else {
                check(0, "iso may have no targets");
            }
            if (vapor_manifest_parse(zip_empty, strlen(zip_empty), &m, err,
                                     sizeof(err))
                == 0) {
                check(m.ntargets == 0, "zip may have no targets");
                vapor_manifest_free(&m);
            } else {
                check(0, "zip may have no targets");
            }
        }
    }
}

static void
both16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void
both32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
    p[4] = (unsigned char)(v >> 24);
    p[5] = (unsigned char)(v >> 16);
    p[6] = (unsigned char)(v >> 8);
    p[7] = (unsigned char)v;
}

static void
dirent_dot(unsigned char *p, uint32_t lba, uint32_t size, int parent)
{
    memset(p, 0, 34);
    p[0] = 34;
    both32(p + 2, lba);
    both32(p + 10, size);
    p[25] = 0x02;
    p[32] = 1;
    p[33] = parent ? 1 : 0;
}

static void
test_iso9660(void)
{
    static const char payload[] = "hello-iso\n";
    unsigned char     img[2048 * 20];
    unsigned char    *pvd, *root, *file_rec;
    FILE             *f;
    char              err[128];
    char              got[32];
    const char       *iso_path = "vapor-iso-selftest.iso";
    const char       *out_dir = "vapor-iso-selftest-out";
    const char       *out_file = "vapor-iso-selftest-out/HELLO.TXT";

    puts("iso9660");
    memset(img, 0, sizeof(img));
    pvd = img + 16 * 2048;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memcpy(pvd + 40, "VAPORTEST", 9);
    both32(pvd + 80, 20);
    both16(pvd + 128, 2048);
    dirent_dot(pvd + 156, 18, 2048, 0);

    img[17 * 2048] = 255;
    memcpy(img + 17 * 2048 + 1, "CD001", 5);
    img[17 * 2048 + 6] = 1;

    root = img + 18 * 2048;
    dirent_dot(root, 18, 2048, 0);
    dirent_dot(root + 34, 18, 2048, 1);
    file_rec = root + 68;
    memset(file_rec, 0, 48);
    file_rec[0] = 44;
    both32(file_rec + 2, 19);
    both32(file_rec + 10, (uint32_t)(sizeof(payload) - 1));
    file_rec[32] = 11;
    memcpy(file_rec + 33, "HELLO.TXT;1", 11);

    memcpy(img + 19 * 2048, payload, sizeof(payload) - 1);

    f = fopen(iso_path, "wb");
    check(f != NULL, "writes a test ISO");
    if (!f) {
        return;
    }
    check(fwrite(img, 1, sizeof(img), f) == sizeof(img), "ISO bytes written");
    fclose(f);

    check(vapor_iso_extract("not-a-real-iso-file", out_dir, err, sizeof(err))
              != 0,
          "rejects missing ISO");
    check(vapor_iso_extract(iso_path, out_dir, err, sizeof(err)) == 0,
          "extracts a minimal ISO");
    f = fopen(out_file, "rb");
    check(f != NULL, "extracted HELLO.TXT");
    if (f) {
        size_t n = fread(got, 1, sizeof(got) - 1, f);
        got[n] = '\0';
        fclose(f);
        check(strcmp(got, payload) == 0, "ISO file contents");
    }
    remove(out_file);
    remove(iso_path);
#if defined(_WIN32)
    _rmdir(out_dir);
#else
    rmdir(out_dir);
#endif
}

static void
put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void
put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static int
deflate_raw(const void *src, size_t n, unsigned char **out, size_t *out_len)
{
    *out = (unsigned char *)tdefl_compress_mem_to_heap(src, n, out_len,
                                                       TDEFL_DEFAULT_MAX_PROBES);
    return (*out && *out_len) ? 0 : -1;
}

static void
test_wise(void)
{
    static const char hello[] = "hello-wise\n";
    static const char dib[] = "DIBDUMMY";
    unsigned char     script[128];
    unsigned char    *dib_c = NULL, *hello_c = NULL, *script_c = NULL;
    unsigned char    *pe = NULL, *ov, *wp;
    size_t            dib_n = 0, hello_n = 0, script_n = 0, ov_len, pe_len;
    uint32_t          crc, e_lfanew = 0x80, optsz = 224, raw_ptr = 0x200;
    uint32_t          raw_sz = 0x200, overlay_off, sec_off;
    FILE             *f;
    char              err[128];
    char              got[32];
    const char       *exe_path = "vapor-wise-selftest.exe";
    const char       *out_dir = "vapor-wise-selftest-out";
    const char       *out_file = "vapor-wise-selftest-out/hello.txt";

    puts("wise");
    check(vapor_wise_extract("not-a-real-setup.exe", out_dir, err, sizeof(err))
              == 1,
          "rejects missing installer");

    check(deflate_raw(dib, sizeof(dib) - 1, &dib_c, &dib_n) == 0, "deflates dib");
    check(deflate_raw(hello, sizeof(hello) - 1, &hello_c, &hello_n) == 0,
          "deflates payload");
    if (!dib_c || !hello_c) {
        return;
    }
    crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char *)hello,
                             sizeof(hello) - 1);
    memset(script, 0, sizeof(script));
    script[16] = 0x00;
    script[17] = 0x80;
    script[18] = 0x00;
    put_u32(script + 19, 0);
    put_u32(script + 23, (uint32_t)(hello_n + 4));
    put_u32(script + 31, (uint32_t)(sizeof(hello) - 1));
    put_u32(script + 55, crc);
    memcpy(script + 59, "%MAINDIR%\\hello.txt", 20);
    check(deflate_raw(script, 80, &script_c, &script_n) == 0, "deflates script");
    if (!script_c) {
        mz_free(dib_c);
        mz_free(hello_c);
        return;
    }

    overlay_off = raw_ptr + raw_sz;
    ov_len = 64 + 40 + dib_n + 4 + script_n + 4 + hello_n + 4;
    pe_len = overlay_off + ov_len + 16;
    pe = (unsigned char *)calloc(1, pe_len);
    check(pe != NULL, "allocates mini PE");
    if (!pe) {
        mz_free(dib_c);
        mz_free(hello_c);
        mz_free(script_c);
        return;
    }
    pe[0] = 'M';
    pe[1] = 'Z';
    put_u32(pe + 0x3c, e_lfanew);
    memcpy(pe + e_lfanew, "PE\0\0", 4);
    put_u16(pe + e_lfanew + 4, 0x14c);
    put_u16(pe + e_lfanew + 6, 1);
    put_u16(pe + e_lfanew + 20, (uint16_t)optsz);
    put_u16(pe + e_lfanew + 22, 0x0102);
    put_u16(pe + e_lfanew + 24, 0x10b);
    sec_off = e_lfanew + 24 + optsz;
    memcpy(pe + sec_off, ".text", 5);
    put_u32(pe + sec_off + 8, raw_sz);
    put_u32(pe + sec_off + 12, 0x1000);
    put_u32(pe + sec_off + 16, raw_sz);
    put_u32(pe + sec_off + 20, raw_ptr);

    ov = pe + overlay_off;
    wp = ov + 32;
    memcpy(wp, "Initializing Wise Installation Wizard", 37);
    wp += 38;
    memcpy(wp, dib_c, dib_n);
    wp += dib_n;
    put_u32(wp, (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char *)dib,
                                   sizeof(dib) - 1));
    wp += 4;
    memcpy(wp, script_c, script_n);
    wp += script_n;
    put_u32(wp, (uint32_t)mz_crc32(MZ_CRC32_INIT, script, 80));
    wp += 4;
    memcpy(wp, hello_c, hello_n);
    wp += hello_n;
    put_u32(wp, crc);

    f = fopen(exe_path, "wb");
    check(f != NULL, "writes a test Wise installer");
    if (f) {
        size_t n = (size_t)(wp - pe);
        check(fwrite(pe, 1, n, f) == n, "Wise PE bytes written");
        fclose(f);
    }
    check(vapor_wise_extract(exe_path, out_dir, err, sizeof(err)) == 0,
          "extracts a minimal Wise installer");
    f = fopen(out_file, "rb");
    check(f != NULL, "extracted hello.txt");
    if (f) {
        size_t n = fread(got, 1, sizeof(got) - 1, f);
        got[n] = '\0';
        fclose(f);
        check(strcmp(got, hello) == 0, "Wise file contents");
    }
    remove(out_file);
    remove(exe_path);
#if defined(_WIN32)
    _rmdir(out_dir);
#else
    rmdir(out_dir);
#endif
    mz_free(dib_c);
    mz_free(hello_c);
    mz_free(script_c);
    free(pe);
}

static void
test_disc_finish_install(void)
{
    const char *dir = "vapor-disc-selftest-out";
    const char *mod = "vapor-disc-selftest-out/mod";
    const char *stub = "vapor-disc-selftest-out/GAME.DAT";
    const char *real = "vapor-disc-selftest-out/mod/GAME.DAT";
    const char *orphan = "vapor-disc-selftest-out/orphan.dat";
    const char *autorun = "vapor-disc-selftest-out/AUTORUN.EXE";
    const char *keep = "vapor-disc-selftest-out/keep.dat";
    FILE       *f;
    unsigned char buf[128];

    puts("disc_finish_install");
#if defined(_WIN32)
    _mkdir(dir);
    _mkdir(mod);
#else
    mkdir(dir, 0755);
    mkdir(mod, 0755);
#endif
    f = fopen(stub, "wb");
    check(f != NULL, "writes tiny root GAME.DAT");
    if (f) {
        fwrite("stub", 1, 4, f);
        fclose(f);
    }
    memset(buf, 0xab, sizeof(buf));
    f = fopen(real, "wb");
    check(f != NULL, "writes larger GAME.DAT in a subdirectory");
    if (f) {
        fwrite(buf, 1, sizeof(buf), f);
        fclose(f);
    }
    f = fopen(orphan, "wb");
    check(f != NULL, "writes orphan tiny dat");
    if (f) {
        fwrite("x", 1, 1, f);
        fclose(f);
    }
    f = fopen(autorun, "wb");
    check(f != NULL, "writes AUTORUN.EXE");
    if (f) {
        fwrite("x", 1, 1, f);
        fclose(f);
    }
    f = fopen(keep, "wb");
    check(f != NULL, "writes a large root dat");
    if (f) {
        fwrite(buf, 1, sizeof(buf), f);
        fclose(f);
    }

    vapor_disc_finish_install(dir);

    f = fopen(stub, "rb");
    check(f == NULL, "removes tiny root dat with a larger namesake");
    if (f) {
        fclose(f);
    }
    f = fopen(real, "rb");
    check(f != NULL, "keeps the larger namesake");
    if (f) {
        fclose(f);
    }
    f = fopen(orphan, "rb");
    check(f != NULL, "leaves a tiny dat with no namesake");
    if (f) {
        fclose(f);
    }
    f = fopen(autorun, "rb");
    check(f == NULL, "removes AUTORUN.EXE");
    if (f) {
        fclose(f);
    }
    f = fopen(keep, "rb");
    check(f != NULL, "leaves a large root dat");
    if (f) {
        fclose(f);
    }

    remove(stub);
    remove(real);
    remove(orphan);
    remove(autorun);
    remove(keep);
#if defined(_WIN32)
    _rmdir(mod);
    _rmdir(dir);
#else
    rmdir(mod);
    rmdir(dir);
#endif
}

int
main(void)
{
    printf("vapor common selftest (host: %s/%s)\n\n",
           vapor_host_platform(), vapor_host_arch());

    test_sha256();
    test_version_cmp();
    test_id_validation();
    test_id_slug();
    test_username_validation();
    test_glob();
    test_buf();
    test_manifest_roundtrip();
    test_iso9660();
    test_wise();
    test_disc_finish_install();

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
