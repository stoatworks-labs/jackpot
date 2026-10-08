/*
 * Stoatworks Labs - About window data for Jackpot.
 *
 * A PROVISIONAL HAND COPY of what stoatworks-backend/scripts/sync-about.py
 * writes, adapted from polyhedral's on 2026-10-08: Jackpot is not in the
 * website's projects.json yet. Registering it replaces this file with the
 * generated one; until then there is no user guide, so `guide` is empty and
 * the About block has three buttons (AGENTS.md).
 *
 * `version` here is a fallback read from this repo's own manifest at sync
 * time. Anything with a build step injects the real one at build time and
 * overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Jackpot";
    inline constexpr auto slug = "jackpot";
    inline constexpr auto hook = "Casino games decided before they are shown, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/jackpot/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/jackpot";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
