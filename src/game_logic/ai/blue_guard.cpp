/* Copyright (C) 2016, Nikolai Wuttke. All rights reserved.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "blue_guard.hpp"

#include "engine/life_time_components.hpp"
#include "engine/random_number_generator.hpp"
#include "engine/sprite_tools.hpp"
#include "engine/visual_components.hpp"
#include "game_logic/entity_factory.hpp"
#include "game_logic/player/components.hpp"

#include "game_mode.hpp"

#include <boost/optional.hpp>


namespace rigel { namespace game_logic { namespace ai {

using engine::components::Active;
using engine::components::BoundingBox;
using engine::components::Sprite;
using engine::components::WorldPosition;
using game_logic::components::PlayerControlled;


namespace {

using Orientation = components::BlueGuard::Orientation;

const auto SPRITE_ORIENTATION_OFFSET = 6;
const auto TYPING_BASE_FRAME = 12;
const auto GUARD_WIDTH = 3;


bool playerInNoticeableRange(
  const WorldPosition& myPosition,
  const WorldPosition& playerPosition
) {
  const auto playerCenterX = playerPosition.x + 1;
  const auto myCenterX = myPosition.x + GUARD_WIDTH/2;
  const auto centerToCenterDistance = std::abs(playerCenterX - myCenterX);

  return
    myPosition.y == playerPosition.y &&
    centerToCenterDistance <= 6;
}


bool playerVisible(
  components::BlueGuard& state,
  const WorldPosition& myPosition,
  const WorldPosition& playerPosition,
  const PlayerControlled& playerState
) {
  const auto playerX = playerPosition.x;
  const auto playerY = playerPosition.y;
  const auto facingLeft = state.mOrientation == Orientation::Left;

  const auto hasLineOfSightHorizontal =
    (facingLeft && myPosition.x >= playerX) ||
    (!facingLeft && myPosition.x <= playerX);
  const auto hasLineOfSightVertical =
    playerY - 3 <= myPosition.y &&
    playerY + 3 >= myPosition.y;

  // TODO: Check player not cloaked
  return
    playerState.isPlayerOnGround() &&
    hasLineOfSightHorizontal &&
    hasLineOfSightVertical;
}


base::Vector offsetForShot(const components::BlueGuard& state) {
  const auto offsetY = state.mIsCrouched ? -1 : -2;
  const auto facingLeft = state.mOrientation == Orientation::Left;
  const auto offsetX = facingLeft ? -1 : 3;

  return {offsetX, offsetY};
}


auto oppositeOrientation(const Orientation orientation) {
  if (orientation == Orientation::Left) {
    return Orientation::Right;
  }

  return Orientation::Left;
}


boost::optional<WorldPosition> walk(
  const WorldPosition& currentPosition,
  const base::Vector& walkVector,
  const BoundingBox& bbox,
  const data::map::Map& map
) {
  const auto newPosition = currentPosition + walkVector;

  const auto movingLeft = walkVector.x < 0;
  const auto xToTest = newPosition.x + (movingLeft ? 0 : bbox.size.width - 1);

  const auto stillOnSolidGround =
    map.collisionData(xToTest, newPosition.y + 1).isSolidTop();

  // TODO: Unify this with the code in the physics system
  bool collidingWithWorld = false;
  for (int i = 0; i < bbox.size.height; ++i) {
    if (!map.collisionData(xToTest, newPosition.y - i).isClear()) {
      collidingWithWorld = true;
      break;
    }
  }

  if (stillOnSolidGround && !collidingWithWorld) {
    return newPosition;
  }

  return boost::none;
}

}


BlueGuardSystem::BlueGuardSystem(
  entityx::Entity player,
  const data::map::Map* pMap,
  EntityFactory* pEntityFactory,
  IGameServiceProvider* pServiceProvider,
  engine::RandomNumberGenerator* pRandomGenerator
)
  : mPlayer(player)
  , mpMap(pMap)
  , mpEntityFactory(pEntityFactory)
  , mpServiceProvider(pServiceProvider)
  , mpRandomGenerator(pRandomGenerator)
{
}


void BlueGuardSystem::update(entityx::EntityManager& es) {
  const auto& playerPosition = *mPlayer.component<WorldPosition>();

  es.each<components::BlueGuard, Sprite, WorldPosition, BoundingBox, Active>(
    [this, &playerPosition](
      entityx::Entity entity,
      components::BlueGuard& state,
      Sprite& sprite,
      WorldPosition& position,
      BoundingBox& bbox,
      const Active&
    ) {
      if (state.mTypingOnTerminal) {
        const auto noticesPlayer =
          playerInNoticeableRange(position, playerPosition);

        if (noticesPlayer) {
          stopTyping(state, sprite, position);
          updateGuard(state, sprite, position, bbox);
        } else {
          // Animate typing on terminal
          const auto skipOneMove = (mpRandomGenerator->gen() & 4) != 0;
          const auto moveHand = mIsOddFrame && !skipOneMove;
          const auto typingFrameOffset = moveHand ? 1 : 0;

          sprite.mFramesToRender[0] = TYPING_BASE_FRAME + typingFrameOffset;
        }
      } else {
        updateGuard(state, sprite, position, bbox);
      }

      engine::synchronizeBoundingBoxToSprite(entity);
    });

  mIsOddFrame = !mIsOddFrame;
}


void BlueGuardSystem::onEntityHit(entityx::Entity entity) {
  if (entity.has_component<components::BlueGuard>()) {
    auto& state = *entity.component<components::BlueGuard>();

    if (state.mTypingOnTerminal) {
      stopTyping(
        state,
        *entity.component<Sprite>(),
        *entity.component<WorldPosition>());
    }
  }
}


void BlueGuardSystem::stopTyping(
  components::BlueGuard& state,
  engine::components::Sprite& sprite,
  WorldPosition& position
) {
  state.mTypingOnTerminal = false;

  // TODO: Use orientation-dependent position here
  const auto playerX = mPlayer.component<WorldPosition>()->x;
  state.mOrientation = position.x <= playerX
    ? Orientation::Right
    : Orientation::Left;

  const auto orientationOffset =
    state.mOrientation == Orientation::Left ? SPRITE_ORIENTATION_OFFSET : 0;
  sprite.mFramesToRender[0] = 0 + orientationOffset;
}


void BlueGuardSystem::updateGuard(
  components::BlueGuard& state,
  engine::components::Sprite& sprite,
  WorldPosition& position,
  const BoundingBox& bbox
) {
  const auto& playerPosition = *mPlayer.component<WorldPosition>();
  const auto& playerState =
    *mPlayer.component<game_logic::components::PlayerControlled>();

  const auto walkOneStep = [this, &state, &position, &bbox]() {
    const auto facingLeft = state.mOrientation == Orientation::Left;
    const auto walkVector = base::Vector{1, 0} * (facingLeft ? -1 : 1);

    const auto newPosition = walk(position, walkVector, bbox, *mpMap);
    if (newPosition) {
      position = *newPosition;
    }

    return newPosition != boost::none;
  };

  if (playerVisible(state, position, playerPosition, playerState)) {
    // Change stance if necessary
    if (state.mStanceChangeCountdown <= 0) {
      const auto playerCrouched =
        playerState.mState == player::PlayerState::Crouching;
      const auto playerBelow = playerPosition.y > position.y;
      state.mIsCrouched = playerCrouched || playerBelow;

      if (state.mIsCrouched) {
        state.mStanceChangeCountdown = mpRandomGenerator->gen() & 0xF;
      }
    } else {
      --state.mStanceChangeCountdown;
    }

    // Fire gun
    const auto facingLeft = state.mOrientation == Orientation::Left;
    const auto wantsToShoot = (mpRandomGenerator->gen() & 7) == 0;
    if (wantsToShoot) {
      // TODO: Only play sound if visible on screen
      mpServiceProvider->playSound(data::SoundId::EnemyLaserShot);
      mpEntityFactory->createProjectile(
        ProjectileType::EnemyLaserShot,
        position + offsetForShot(state),
        facingLeft ? ProjectileDirection::Left : ProjectileDirection::Right);
    }

    // Update sprite
    if (wantsToShoot && !state.mIsCrouched) {
      // Show gun recoil animation in non-crouched state
      sprite.mFramesToRender[0] = facingLeft ? 15 : 14;
    } else {
      const auto animationFrame = state.mIsCrouched ? 5 : 4;
      const auto orientationOffset =
        facingLeft ? SPRITE_ORIENTATION_OFFSET : 0;
      sprite.mFramesToRender[0] = animationFrame + orientationOffset;
    }
  } else {
    state.mStanceChangeCountdown = 0;

    if (mIsOddFrame) {
      const auto walkedSuccessfully = walkOneStep();

      ++state.mStepsWalked;
      if (state.mStepsWalked >= 20 || !walkedSuccessfully) {
        state.mOrientation = oppositeOrientation(state.mOrientation);

        // After changing orientation, walk one step in the new direction on
        // the same frame. The original code used a jump to accomplish this,
        // which means you can get into an infinite loop in the original game
        // by placing a blue guard in the right situation (no move possible).
        walkOneStep();
        state.mStepsWalked = 1;
      }
    }

    // Update sprite
    const auto walkAnimationFrame = state.mStepsWalked % 4;
    const auto orientationOffset =
      state.mOrientation == Orientation::Left ? SPRITE_ORIENTATION_OFFSET : 0;
    sprite.mFramesToRender[0] = walkAnimationFrame + orientationOffset;
  }
}

}}}
