// test_h2_router.cpp
// Unit tests for h2::Router (Task 4.1).
//
// Tests: exact match, prefix match, 404, longest-prefix wins, implicit
// redirect, user-defined route blocks auto-redirect, duplicate registration.
//
// No nghttp2 dependency — h2_router.cpp uses only h2_request.hpp /
// h2_response.hpp types.  Minimal stubs for the two Response methods called
// by the implicit-redirect lambda are provided at the bottom of this file.

#include <gtest/gtest.h>
#include "h2_router.hpp"

// ---------------------------------------------------------------------------
// Free-function handlers for identity testing via std::function::target<>.
// Using named free functions lets us extract a typed pointer from the
// std::function and compare it directly without invoking the handler.
// ---------------------------------------------------------------------------
static void handler_A(const h2::Request&, h2::Response&) {}
static void handler_B(const h2::Request&, h2::Response&) {}

using FPtr = void (*)(const h2::Request&, h2::Response&);

// Extract the underlying function pointer from a RequestHandler, or nullptr
// if the handler is empty or holds a different callable type.
static FPtr target_of(const h2::RequestHandler& h) {
    if (!h) return nullptr;
    const FPtr* p = h.target<FPtr>();
    return p ? *p : nullptr;
}

// ===========================================================================
// Test 1: Exact match found.
// ===========================================================================
TEST(Router, ExactMatchFound) {
    h2::Router r;
    EXPECT_TRUE(r.handle("/foo", handler_A));
    EXPECT_TRUE(static_cast<bool>(r.match("/foo")));
}

// ===========================================================================
// Test 2: Exact match not found for a different path.
// ===========================================================================
TEST(Router, ExactMatchNotFound) {
    h2::Router r;
    r.handle("/foo", handler_A);
    EXPECT_FALSE(static_cast<bool>(r.match("/bar")));
}

// ===========================================================================
// Test 3: No routes → match returns null for any path.
// ===========================================================================
TEST(Router, NoMatchOnEmptyRouter) {
    h2::Router r;
    EXPECT_FALSE(static_cast<bool>(r.match("/anything")));
    EXPECT_FALSE(static_cast<bool>(r.match("")));
}

// ===========================================================================
// Test 4: Prefix match — trailing-slash pattern matches sub-paths.
// ===========================================================================
TEST(Router, PrefixMatchFound) {
    h2::Router r;
    r.handle("/api/", handler_A);
    EXPECT_TRUE(static_cast<bool>(r.match("/api/v1/data")));
    EXPECT_TRUE(static_cast<bool>(r.match("/api/")));  // exact prefix hit
}

// ===========================================================================
// Test 5: Prefix patterns do NOT match unrelated or too-short paths.
//
// Note: handle("/api/") auto-installs a redirect for "/api" (see
// ImplicitRedirectInstalled test), so "/api" is intentionally NOT tested
// here — it does produce a non-null handler (the redirect).
// ===========================================================================
TEST(Router, PrefixNoMatchForUnrelatedPaths) {
    h2::Router r;
    r.handle("/api/", handler_A);
    EXPECT_FALSE(static_cast<bool>(r.match("/ap")));      // too short, no redirect
    EXPECT_FALSE(static_cast<bool>(r.match("/other")));   // unrelated path
    EXPECT_FALSE(static_cast<bool>(r.match("")));         // empty path
}

// ===========================================================================
// Test 6: Exact match takes priority over a matching prefix.
// ===========================================================================
TEST(Router, ExactMatchPriorityOverPrefix) {
    h2::Router r;
    r.handle("/api/",  handler_A);   // prefix
    r.handle("/api/v1", handler_B);  // exact

    // /api/v1 is an exact match → handler_B.
    EXPECT_EQ(target_of(r.match("/api/v1")), handler_B);

    // /api/v2 has no exact match — falls back to prefix /api/ → handler_A.
    EXPECT_EQ(target_of(r.match("/api/v2")), handler_A);
}

// ===========================================================================
// Test 7: Longer prefix wins over shorter prefix (most-specific).
// ===========================================================================
TEST(Router, LongerPrefixWins) {
    h2::Router r;
    r.handle("/a/",   handler_A);
    r.handle("/a/b/", handler_B);

    EXPECT_EQ(target_of(r.match("/a/b/c")), handler_B);  // longer prefix
    EXPECT_EQ(target_of(r.match("/a/c")),   handler_A);  // only shorter prefix matches
}

// ===========================================================================
// Test 8: Registering "/foo/" auto-installs a 301-redirect for "/foo".
// ===========================================================================
TEST(Router, ImplicitRedirectInstalled) {
    h2::Router r;
    r.handle("/foo/", handler_A);

    // The redirect handler (auto-installed) should be present for "/foo".
    EXPECT_TRUE(static_cast<bool>(r.match("/foo")));
    // The original handler is still present for "/foo/".
    EXPECT_TRUE(static_cast<bool>(r.match("/foo/")));
}

// ===========================================================================
// Test 9: A user-defined "/foo" blocks the auto-redirect from "/foo/".
// ===========================================================================
TEST(Router, UserDefinedTakesPriorityOverAutoRedirect) {
    h2::Router r;
    r.handle("/foo",  handler_A);  // user registers /foo first
    r.handle("/foo/", handler_B);  // /foo/ should not overwrite /foo

    // /foo must return the user-defined handler (handler_A), not the redirect.
    EXPECT_EQ(target_of(r.match("/foo")), handler_A);
    // /foo/ returns handler_B.
    EXPECT_EQ(target_of(r.match("/foo/")), handler_B);
}

// ===========================================================================
// Test 10: Registering the same user-defined pattern twice returns false.
// ===========================================================================
TEST(Router, DuplicateRegistrationReturnsFalse) {
    h2::Router r;
    EXPECT_TRUE(r.handle("/dup", handler_A));
    EXPECT_FALSE(r.handle("/dup", handler_B));  // second registration rejected

    // First handler is still in place.
    EXPECT_EQ(target_of(r.match("/dup")), handler_A);
}

// ===========================================================================
// Test 11: Empty pattern and null handler are rejected.
// ===========================================================================
TEST(Router, InvalidHandleCallsRejected) {
    h2::Router r;
    EXPECT_FALSE(r.handle("", handler_A));   // empty pattern
    EXPECT_FALSE(r.handle("/ok", nullptr));  // null handler
}

// ===========================================================================
// Stubs: provide weak definitions for the Response methods invoked by the
// implicit-redirect lambda inside h2_router.cpp.  When compiled together
// with h2_response.cpp (which provides the strong definitions) the linker
// silently discards these weak symbols — no multiple-definition error.
// When compiled without h2_response.cpp the weak symbols satisfy the
// undefined references.
// ===========================================================================
#include "h2_response.hpp"

namespace h2 {

__attribute__((weak))
void Response::write_head(unsigned int /*status_code*/,
                          const HeaderMap& /*headers*/) {}

__attribute__((weak))
void Response::end() {}

}  // namespace h2
