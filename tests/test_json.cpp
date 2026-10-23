// test_json.cpp — the JSON reader/writer: valid and invalid documents, exact float
// round trips, and fuzzing (every document that parses must also round-trip).
#include <cmath>
#include <cstring>
#include <random>
#include <string>

#include "check.h"
#include "json.h"

namespace {

bool parses(const std::string& text) {
    Json j;
    std::string error;
    return Json::parse(text, j, error);
}

void testValid() {
    Json j;
    std::string error;
    CHECK_MSG(Json::parse(R"( { "a": [1, -0.5, 2e3, 1E-2, true, false, null], "b": {"c": "x\"y\\z\n"} } )", j, error),
              "%s", error.c_str());
    CHECK(j.isObject() && j.find("a") && j.find("a")->size() == 7);
    CHECK((*j.find("a"))[2].asNumber() == 2000 && (*j.find("a"))[3].asNumber() == 0.01);
    CHECK(j.find("b")->find("c")->asString() == "x\"y\\z\n");
    CHECK(j.find("missing") == nullptr);
    // \u escapes: BMP (é) and a surrogate pair (U+1F600) both become UTF-8.
    CHECK(Json::parse(R"("\u00e9 \ud83d\ude00")", j, error) && j.asString() == "\xC3\xA9 \xF0\x9F\x98\x80");
    // Key order is preserved, and a repeated key keeps its first position.
    CHECK(Json::parse(R"({"z":1,"a":2,"z":3})", j, error) && j.members().size() == 2 && j.members()[0].first == "z" &&
          j.members()[0].second.asNumber() == 3);
}

void testInvalid() {
    const char* bad[] = {"", "nul", "[1,]", "{\"a\":1,}", "01", "1.", ".5", "+1", "1e", "\"abc", "\"a\nb\"",
                         "\"\\x\"", "\"\\ud800\"", "\"\\udc00\"", "[1 2]", "{\"a\" 1}", "{1:2}", "1 2", "NaN", "1e400"};
    for (const char* text : bad) CHECK_MSG(!parses(text), "accepted invalid JSON: %s", text);
    std::string deep(10000, '[');  // would overflow the stack without the depth limit
    CHECK(!parses(deep));
    CHECK(parses(std::string(200, '[') + std::string(200, ']')));
}

void testFloats() {
    CHECK(Json::fromFloat(0.1f).dump() == "0.1");
    CHECK(Json::fromFloat(1.0f).dump() == "1");
    CHECK(Json::fromFloat(-2.5f).dump() == "-2.5");
    std::mt19937 rng(11);
    int mismatches = 0;
    for (int i = 0; i < 100000; ++i) {
        uint32_t bits = rng();
        float f;
        std::memcpy(&f, &bits, 4);
        if (!std::isfinite(f)) continue;
        Json back;
        std::string error;
        if (!Json::parse(Json::fromFloat(f).dump(), back, error) || static_cast<float>(back.asNumber()) != f)
            ++mismatches;
    }
    CHECK_MSG(mismatches == 0, "%d floats didn't survive a save/load round trip", mismatches);
}

void testRoundTripAndFuzz() {
    Json doc = Json::object();
    doc.set("name", "Cube \xC3\xA9");
    doc.set("ints", Json::array());
    Json& arr = const_cast<Json&>(*doc.find("ints"));
    for (int i = -3; i < 4; ++i) arr.push(Json(i * 1000003));
    doc.set("pi", Json(3.14159265358979));
    doc.set("nested", Json::object()).set("deep", Json::array()).push(Json(true));
    for (int indent : {0, 2}) {
        Json back;
        std::string error;
        CHECK(Json::parse(doc.dump(indent), back, error) && back == doc);
    }
    const std::string seed = doc.dump(2);
    std::mt19937 rng(5);
    const char alphabet[] = "{}[]\",:0123456789-+.eE \\untrlfas\n";
    int parsed = 0;
    for (int t = 0; t < 20000; ++t) {
        std::string s = seed;
        for (int k = 0, n = 1 + rng() % 4; k < n; ++k) {
            const size_t at = rng() % (s.size() + 1);
            switch (rng() % 3) {
                case 0: if (at < s.size()) s[at] = alphabet[rng() % (sizeof(alphabet) - 1)]; break;
                case 1: s.insert(at, 1, alphabet[rng() % (sizeof(alphabet) - 1)]); break;
                default: if (at < s.size()) s.erase(at, 1);
            }
        }
        Json j, back;
        std::string error;
        if (!Json::parse(s, j, error)) continue;
        ++parsed;
        CHECK(Json::parse(j.dump(), back, error) && back == j);  // anything we accept, we can write back exactly
    }
    CHECK(parsed > 100);  // the fuzzer really exercises the success path too
}

}  // namespace

int main() {
    testValid();
    testInvalid();
    testFloats();
    testRoundTripAndFuzz();
    return testResult("test_json");
}
