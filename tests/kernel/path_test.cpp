#include <kernel/path.hpp>

#include <support/test.hpp>

namespace {

using namespace mikos;

[[nodiscard]] bool text_is(const char* actual, const char* expected) {
  while (*actual == *expected) {
    if (*actual == '\0') {
      return true;
    }
    ++actual;
    ++expected;
  }
  return false;
}

void check(mikos::test::Suite& suite, const char* cwd, const char* input,
           const char* expected) {
  char output[path::capacity]{};
  MIKOS_CHECK(suite,
              path::canonicalize(cwd, input, output, sizeof(output)));
  MIKOS_CHECK(suite, text_is(output, expected));
}

}  // namespace

int main() {
  mikos::test::Suite suite{"kernel/path"};
  check(suite, "/", "/", "/");
  check(suite, "/", "bin", "/bin");
  check(suite, "/bin", "./stress-ng", "/bin/stress-ng");
  check(suite, "/usr/share", "../bin/./tool", "/usr/bin/tool");
  check(suite, "/bin", "../../etc//issue", "/etc/issue");
  check(suite, "/ignored", "//bin///busybox", "/bin/busybox");

  char output[4]{};
  MIKOS_CHECK(suite, !path::canonicalize("/", "tool", output,
                                         sizeof(output)));
  MIKOS_CHECK(suite, !path::canonicalize("/bin", "", output,
                                         sizeof(output)));
  return suite.finish();
}
