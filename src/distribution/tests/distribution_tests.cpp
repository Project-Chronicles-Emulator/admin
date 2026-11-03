#include "distribution/distribution.hpp"

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <array>
#include <memory>

namespace chronicles::distribution {
namespace {

using json = nlohmann::json;

std::string hexadecimal(const unsigned char *data, std::size_t size) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text;
  for (std::size_t index = 0; index < size; ++index) {
    text.push_back(digits[data[index] >> 4]);
    text.push_back(digits[data[index] & 15]);
  }
  return text;
}

class SignedReleaseTest : public testing::Test {
protected:
  SignedReleaseTest() {
    std::array<unsigned char, 32> seed{};
    seed.fill(7);
    key_.reset(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
    std::array<unsigned char, 32> public_bytes{};
    std::size_t public_size = public_bytes.size();
    if (!key_ || EVP_PKEY_get_raw_public_key(key_.get(), public_bytes.data(), &public_size) != 1) {
      throw std::runtime_error("Cannot initialize the synthetic signing fixture");
    }
    public_key_ = hexadecimal(public_bytes.data(), public_size);
  }

  json manifest() const {
    return {{"application", "chronicles"},
            {"format", 1},
            {"channel", "stable"},
            {"version", "fixture-1"},
            {"sequence", 1},
            {"api_origin", "https://realm.example"},
            {"files",
             json::array(
                 {{{"path", "Game/content.bin"}, {"size", 3}, {"sha256", sha256_bytes("abc")}}})}};
  }

  std::string sign(const json &manifest) const {
    const auto payload = manifest.dump();
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    std::array<unsigned char, 64> signature{};
    std::size_t signature_size = signature.size();
    if (EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key_.get()) != 1 ||
        EVP_DigestSign(context.get(), signature.data(), &signature_size,
                       reinterpret_cast<const unsigned char *>(payload.data()),
                       payload.size()) != 1) {
      throw std::runtime_error("Cannot sign synthetic manifest");
    }
    return json{{"payload", payload}, {"signature", hexadecimal(signature.data(), signature_size)}}
        .dump();
  }

  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_{nullptr, EVP_PKEY_free};
  std::string public_key_;
};

TEST(DistributionOrigin, RequiresHttpsOffLoopbackAndRejectsUserinfoOrPaths) {
  EXPECT_EQ(parse_service_origin("http://127.0.0.1:8480/").port, 8480);
  EXPECT_TRUE(parse_service_origin("https://realm.example").secure);
  EXPECT_TRUE(parse_service_origin("http://[::1]:8480").loopback);
  for (const auto value :
       {"http://realm.example", "http://127.0.0.1.evil.example", "https://user@realm.example",
        "https://realm.example/path", "https://realm.example?token=x", "https://realm.example:0",
        "https://realm.example:65536",
        "https://realm.example:", "https://realm.example/#fragment"}) {
    EXPECT_THROW(parse_service_origin(value), std::invalid_argument) << value;
  }
}

TEST(DistributionPath, RejectsTraversalWindowsAliasesAndInstallerMetadata) {
  EXPECT_TRUE(valid_patch_path("X5/Content/Game Pak-1.pak"));
  for (const auto path :
       {"../outside", "/absolute", "C:/Windows/file", "dir\\file", "file:stream", "a/../b", "a//b",
        "a/NUL.txt", "com1.bin", "trailing.", "trailing ", ".chronicles-install.json",
        ".CHRONICLES-update/journal.json", "%2e%2e/file"}) {
    EXPECT_FALSE(valid_patch_path(path)) << path;
  }
}

TEST_F(SignedReleaseTest, AcceptsExactSignedPayloadAndRejectsTampering) {
  const auto envelope = sign(manifest());
  const auto release = verify_release(envelope, public_key_, "stable", "https://realm.example");
  ASSERT_EQ(release.files.size(), 1);
  EXPECT_EQ(release.files.front().path, "Game/content.bin");
  auto changed = json::parse(envelope);
  changed["payload"] = changed.at("payload").get<std::string>() + " ";
  EXPECT_THROW(verify_release(changed.dump(), public_key_, "stable", "https://realm.example"),
               std::invalid_argument);
  EXPECT_THROW(verify_release(envelope, std::string(64, '0'), "stable", "https://realm.example"),
               std::invalid_argument);
  EXPECT_THROW(verify_release(envelope, public_key_, "beta", "https://realm.example"),
               std::invalid_argument);
  EXPECT_THROW(verify_release(envelope, public_key_, "stable", "https://other.example"),
               std::invalid_argument);
}

TEST_F(SignedReleaseTest, RejectsCaseCollisionsParentConflictsNegativeSizesAndTraversal) {
  auto document = manifest();
  auto second = document["files"][0];
  second["path"] = "game/CONTENT.bin";
  document["files"].push_back(second);
  EXPECT_THROW(verify_release(sign(document), public_key_, "stable", "https://realm.example"),
               std::invalid_argument);
  document = manifest();
  document["files"][0]["path"] = "../outside";
  EXPECT_THROW(verify_release(sign(document), public_key_, "stable", "https://realm.example"),
               std::invalid_argument);
  document = manifest();
  document["files"][0]["size"] = -1;
  EXPECT_THROW(verify_release(sign(document), public_key_, "stable", "https://realm.example"),
               std::invalid_argument);
  document = manifest();
  second["path"] = "Game";
  document["files"].push_back(second);
  EXPECT_THROW(verify_release(sign(document), public_key_, "stable", "https://realm.example"),
               std::invalid_argument);
}

} // namespace
} // namespace chronicles::distribution
