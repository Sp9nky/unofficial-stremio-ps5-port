// Preview only: lets a test finish the sign-in as if the phone had approved it (the session keeps that private).
#include <algorithm>
#include <any>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define private public
#include "session.hpp"
#undef private

namespace sx::preview
{
void test_login_done(Session &session, const std::string &auth_key)
{
    session.login_done(auth_key);
}
} // namespace sx::preview
