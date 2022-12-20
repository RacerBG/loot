/*  LOOT

    A load order optimisation tool for
    Morrowind, Oblivion, Skyrim, Skyrim Special Edition, Skyrim VR,
    Fallout 3, Fallout: New Vegas, Fallout 4 and Fallout 4 VR.

    Copyright (C) 2012 WrinklyNinja

    This file is part of LOOT.

    LOOT is free software: you can redistribute
    it and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.

    LOOT is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with LOOT.  If not, see
    <https://www.gnu.org/licenses/>.
    */

#ifndef LOOT_GUI_STATE_GAME_GAME
#define LOOT_GUI_STATE_GAME_GAME

#include <execution>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

#include "gui/state/game/game_settings.h"
#include "gui/state/logging.h"
#include "loot/api.h"

namespace loot {
namespace gui {
class Game {
public:
  Game(const GameSettings& gameSettings,
       const std::filesystem::path& lootDataPath,
       const std::filesystem::path& preludePath);
  Game(const Game& game) = delete;
  Game(Game&& game);
  ~Game() = default;

  Game& operator=(const Game& game) = delete;
  Game& operator=(Game&& game);

  const GameSettings& GetSettings() const;
  GameSettings& GetSettings();

  void Init();
  bool IsInitialised() const;

  const PluginInterface* GetPlugin(const std::string& name) const;
  std::vector<const PluginInterface*> GetPlugins() const;
  std::vector<Message> CheckInstallValidity(const PluginInterface& plugin,
                                            const PluginMetadata& metadata,
                                            const std::string& language) const;

  void RedatePlugins();  // Change timestamps to match load order (Skyrim only).

  void LoadCreationClubPluginNames();
  bool IsCreationClubPlugin(const PluginInterface& plugin) const;

  void LoadAllInstalledPlugins(
      bool headersOnly);  // Loads all installed plugins.
  bool ArePluginsFullyLoaded()
      const;  // Checks if the game's plugins have already been loaded.

  std::filesystem::path MasterlistPath() const;
  std::filesystem::path UserlistPath() const;
  std::filesystem::path GroupNodePositionsPath() const;
  std::filesystem::path GetActivePluginsFilePath() const;

  std::vector<std::string> GetLoadOrder() const;
  void SetLoadOrder(const std::vector<std::string>& loadOrder);

  bool IsPluginActive(const std::string& pluginName) const;
  std::optional<short> GetActiveLoadOrderIndex(
      const PluginInterface& plugin,
      const std::vector<std::string>& loadOrder) const;

  bool IsLoadOrderAmbiguous() const;

  std::vector<std::string> SortPlugins();
  void IncrementLoadOrderSortCount();
  void DecrementLoadOrderSortCount();

  std::vector<Message> GetMessages() const;
  void AppendMessage(const Message& message);
  void ClearMessages();

  void LoadMetadata();
  std::vector<std::string> GetKnownBashTags() const;

  std::vector<Group> GetMasterlistGroups() const;
  std::vector<Group> GetUserGroups() const;

  std::optional<PluginMetadata> GetMasterlistMetadata(
      const std::string& pluginName,
      bool evaluateConditions = false) const;
  std::optional<PluginMetadata> GetUserMetadata(
      const std::string& pluginName,
      bool evaluateConditions = false) const;
  std::optional<PluginMetadata> GetNonUserMetadata(
      const PluginInterface& plugin) const;

  void SetUserGroups(const std::vector<Group>& groups);
  void AddUserMetadata(const PluginMetadata& metadata);
  void ClearUserMetadata(const std::string& pluginName);
  void ClearAllUserMetadata();
  void SaveUserMetadata();

private:
  std::filesystem::path GetLOOTGamePath() const;
  std::vector<std::string> GetInstalledPluginNames() const;
  void AppendMessages(std::vector<Message> messages);

  GameSettings settings_;
  std::unique_ptr<GameInterface> gameHandle_;
  std::vector<Message> messages_;
  std::filesystem::path lootDataPath_;
  std::filesystem::path preludePath_;
  unsigned short loadOrderSortCount_{0};
  bool pluginsFullyLoaded_{false};

  // Use Filename to benefit from libloot's case-insensitive comparisons.
  std::set<Filename> creationClubPlugins_;

  mutable std::mutex mutex_;
};
}

template<typename T>
std::vector<T> MapFromLoadOrderData(
    const gui::Game& game,
    const std::vector<std::string>& loadOrder,
    const std::function<
        T(const PluginInterface* const, std::optional<short>, bool)>& mapper) {
  typedef std::tuple<const PluginInterface* const, std::optional<short>, bool>
      LoadOrderTuple;

  std::vector<LoadOrderTuple> data;
  data.reserve(loadOrder.size());

  short numberOfActiveLightPlugins = 0;
  short numberOfActiveNormalPlugins = 0;

  // First get all the necessary data to call the mapper, as this is fast.
  for (const auto& pluginName : loadOrder) {
    const auto plugin = game.GetPlugin(pluginName);
    if (!plugin) {
      continue;
    }

    const auto isLight = plugin->IsLightPlugin();
    const auto isActive = game.IsPluginActive(pluginName);

    const auto numberOfActivePlugins =
        isLight ? numberOfActiveLightPlugins : numberOfActiveNormalPlugins;

    const auto activeLoadOrderIndex =
        isActive ? std::optional(numberOfActivePlugins) : std::nullopt;

    data.push_back(std::make_tuple(plugin, activeLoadOrderIndex, isActive));

    if (isActive) {
      if (isLight) {
        ++numberOfActiveLightPlugins;
      } else {
        ++numberOfActiveNormalPlugins;
      }
    }
  }

  // Now perform the mapping in a second loop that can be parallelised
  // (because sometimes the mapper is slow).
  //
  // Store mapped data in an std::variant because if the transformation is
  // fallible then there needs to be some way of detecting that. The second
  // type in the variant holds the exception message string if an exception
  // is thrown by the mapper.
  typedef std::variant<T, std::string> MappedDataOrError;
  const auto transformer = [&mapper](const LoadOrderTuple& loadOrderTuple) {
    try {
      const auto [plugin, activeLoadOrderIndex, isActive] = loadOrderTuple;

      const auto mappedData = mapper(plugin, activeLoadOrderIndex, isActive);

      return MappedDataOrError(mappedData);
    } catch (const std::exception& e) {
      const auto logger = getLogger();
      if (logger) {
        logger->error(
            "Failed to map load order data to output type, exception is: {}",
            e.what());
      }
      return MappedDataOrError(e.what());
    }
  };

  // Can't use std::back_inserter as the output iterator when running the
  // transform in parallel, so presize the vector.
  std::vector<MappedDataOrError> maybeMappedData(data.size());

  std::transform(std::execution::par_unseq,
                 data.cbegin(),
                 data.cend(),
                 maybeMappedData.begin(),
                 transformer);

  std::vector<T> mappedData;
  mappedData.reserve(maybeMappedData.size());

  for (const auto& mappedDataOrError : maybeMappedData) {
    if (std::holds_alternative<T>(mappedDataOrError)) {
      mappedData.push_back(std::get<T>(mappedDataOrError));
    } else {
      throw std::runtime_error(std::get<std::string>(mappedDataOrError));
    }
  }

  return mappedData;
}
}

#endif
