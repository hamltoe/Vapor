/* Covers the shared code where a quiet bug would corrupt installs or let a
 * hostile game id escape the library directory. Run via `ctest`. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vapor/buf.h"
#include "vapor/manifest.h"
#include "vapor/sha256.h"
#include "vapor/util.h"

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

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
