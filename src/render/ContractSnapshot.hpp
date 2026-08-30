#pragma once

#include <SKSE/SKSE.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mfgdlss::render
{
namespace snapshot_detail
{
namespace logger = SKSE::log;
}

class ContractSnapshot final
{
public:
    explicit ContractSnapshot(std::string_view name) : name_{name} {}

    ContractSnapshot(const ContractSnapshot&) = delete;
    ContractSnapshot& operator=(const ContractSnapshot&) = delete;

    void set(std::string_view key, std::string value)
    {
        for (auto& entry : pending_) {
            if (entry.first == key) {
                entry.second = std::move(value);
                return;
            }
        }
        pending_.emplace_back(std::string{key}, std::move(value));
    }

    void set(const std::string_view key, const std::uint64_t value)
    {
        set(key, std::to_string(value));
    }

    void set(const std::string_view key, const std::uint32_t value)
    {
        set(key, std::to_string(value));
    }

    void set(const std::string_view key, const bool value)
    {
        set(key, std::string{value ? "true" : "false"});
    }

    void extent(
        const std::string_view key,
        const std::uint32_t left,
        const std::uint32_t top,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        set(key,
            std::to_string(width) + "x" + std::to_string(height) + " at " +
                std::to_string(left) + "," + std::to_string(top));
    }

    void publish()
    {
        if (pending_.empty()) {
            return;
        }
        std::string changed;
        auto first_publication = published_.empty();
        for (const auto& entry : pending_) {
            const auto* previous = find(published_, entry.first);
            if (previous == nullptr || *previous != entry.second) {
                if (!changed.empty()) {
                    changed += ", ";
                }
                changed += entry.first;
                if (previous != nullptr) {
                    changed += " " + *previous + " -> " + entry.second;
                }
            }
        }
        if (changed.empty()) {
            pending_.clear();
            return;
        }

        std::string body;
        for (const auto& entry : pending_) {
            if (!body.empty()) {
                body += "  ";
            }
            body += entry.first;
            body += "=";
            body += entry.second;
        }

        if (first_publication) {
            snapshot_detail::logger::info(
                "{} contract established: {}", name_, body);
        } else {
            snapshot_detail::logger::info(
                "{} contract changed ({}). Full state now: {}",
                name_,
                changed,
                body);
        }
        published_ = pending_;
        pending_.clear();
    }

    void reset()
    {
        published_.clear();
        pending_.clear();
    }

private:
    using Entry = std::pair<std::string, std::string>;

    [[nodiscard]] static const std::string* find(
        const std::vector<Entry>& entries,
        const std::string& key) noexcept
    {
        for (const auto& entry : entries) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    std::string name_;
    std::vector<Entry> pending_;
    std::vector<Entry> published_;
};
}
