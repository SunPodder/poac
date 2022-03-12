#ifndef POAC_DATA_LOCKFILE_HPP
#define POAC_DATA_LOCKFILE_HPP

// std
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
#include <utility>

// external
#include <fmt/core.h>
#include <mitama/result/result.hpp>
#include <mitama/anyhow/anyhow.hpp>
#include <mitama/thiserror/thiserror.hpp>
#include <toml.hpp>

// internal
#include <poac/core/resolver/resolve.hpp>
#include <poac/data/manifest.hpp>

namespace poac::data::lockfile {
    namespace fs = std::filesystem;
    namespace anyhow = mitama::anyhow;
    namespace thiserror = mitama::thiserror;

    inline const std::string lockfile_name = "poac.lock";
    inline const std::string lockfile_header =
        " This file is automatically generated by Poac.\n"
        "# It is not intended for manual editing.";

    class Error {
        template <thiserror::fixed_string S, class ...T>
        using error = thiserror::error<S, T...>;

    public:
        using InvalidLockfileVersion =
            error<"invalid lockfile version found: {0}", toml::integer>;

        using FailedToReadLockfile =
            error<"failed to read lockfile:\n{0}", std::string>;
    };

    inline fs::file_time_type
    poac_lock_last_modified(const fs::path& base_dir) {
        return fs::last_write_time(base_dir / lockfile_name);
    }
    inline fs::file_time_type
    poac_toml_last_modified(const fs::path& base_dir) {
        return fs::last_write_time(base_dir / manifest::manifest_file_name);
    }

    inline bool
    is_outdated(const fs::path& base_dir) {
        if (!fs::exists(base_dir / lockfile_name)) {
            return true;
        }
        return poac_lock_last_modified(base_dir) < poac_toml_last_modified(base_dir);
    }
} // end namespace

namespace poac::data::lockfile::inline v1 {
    namespace resolver = core::resolver::resolve;

    inline const toml::integer lockfile_version = 1;

    struct package_t {
        toml::string name;
        toml::string version;
        std::vector<std::string> dependencies;
    };

    struct lockfile_t {
        toml::integer version = lockfile_version;
        std::vector<package_t> package;
    };
} // end namespace

TOML11_DEFINE_CONVERSION_NON_INTRUSIVE(
    poac::data::lockfile::v1::package_t, name, version, dependencies
)
TOML11_DEFINE_CONVERSION_NON_INTRUSIVE(
    poac::data::lockfile::v1::lockfile_t, version, package
)

namespace poac::data::lockfile::inline v1 {
    // -------------------- INTO LOCKFILE --------------------

    [[nodiscard]] anyhow::result<toml::basic_value<toml::preserve_comments>>
    convert_to_lock(const resolver::unique_deps_t<resolver::with_deps>& deps) {
        std::vector<package_t> packages;
        for (const auto& [pack, inner_deps] : deps) {
            package_t p{
                resolver::get_name(pack),
                resolver::get_version(pack),
                std::vector<std::string>{},
            };
            if (inner_deps.has_value()) {
                // Extract name from inner dependencies and drop version.
                std::vector<std::string> ideps;
                for (const auto& [name, _v] : inner_deps.value()) {
                    static_cast<void>(_v);
                    ideps.emplace_back(name);
                }
                p.dependencies = ideps;
            }
            packages.emplace_back(p);
        }

        toml::basic_value<toml::preserve_comments> lock(
            lockfile_t{ .package = packages },
            { lockfile_header }
        );
        return mitama::success(lock);
    }

    [[nodiscard]] anyhow::result<void>
    overwrite(const resolver::unique_deps_t<resolver::with_deps>& deps) {
        const auto lock = MITAMA_TRY(convert_to_lock(deps));
        std::ofstream lockfile(config::path::current / lockfile_name, std::ios::out);
        lockfile << lock;
        return mitama::success();
    }

    [[nodiscard]] anyhow::result<void>
    generate(const resolver::unique_deps_t<resolver::with_deps>& deps) {
        if (is_outdated(config::path::current)) {
            return overwrite(deps);
        }
        return mitama::success();
    }

    // -------------------- FROM LOCKFILE --------------------

    [[nodiscard]] resolver::unique_deps_t<resolver::with_deps>
    convert_to_deps(const lockfile_t& lock) {
        resolver::unique_deps_t<resolver::with_deps> deps;
        for (const auto& package : lock.package) {
            resolver::unique_deps_t<resolver::with_deps>::mapped_type inner_deps = std::nullopt;
            if (!package.dependencies.empty()) {
                // When serializing lockfile, package version of inner dependencies
                // will be dropped (ref: `convert_to_lock` function).
                // Thus, the version should be restored just as empty string ("").
                resolver::unique_deps_t<resolver::with_deps>::mapped_type::value_type ideps;
                for (const auto& name : package.dependencies) {
                    ideps.push_back({ name, "" });
                }
            }
            deps.emplace(resolver::package_t{ package.name, package.version }, inner_deps);
        }
        return deps;
    }

    [[nodiscard]] anyhow::result<std::optional<resolver::unique_deps_t<resolver::with_deps>>>
    read(const fs::path& base_dir) {
        if (!fs::exists(base_dir / lockfile_name)) {
            return mitama::success(std::nullopt);
        }

        try {
            const auto lock = toml::parse(base_dir / lockfile_name);
            const auto parsed_lock = toml::get<lockfile_t>(lock);
            if (parsed_lock.version != lockfile_version) {
                return anyhow::failure<Error::InvalidLockfileVersion>(
                    parsed_lock.version
                );
            }
            return mitama::success(convert_to_deps(parsed_lock));
        } catch (const std::exception& e) {
            return anyhow::failure<Error::FailedToReadLockfile>(e.what());
        }
    }
} // end namespace

#endif // !POAC_DATA_LOCKFILE_HPP