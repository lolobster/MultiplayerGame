#include <Book/World.hpp>
#include <Book/Projectile.hpp>
#include <Book/Pickup.hpp>
#include <Book/Foreach.hpp>
#include <Book/TextNode.hpp>
#include <Book/SoundNode.hpp>
#include <Book/NetworkNode.hpp>
#include <Book/Utility.hpp>
#include <SFML/Graphics/RenderTarget.hpp>

#include <algorithm>
#include <cmath>
#include <limits>


World::World(sf::RenderTarget& outputTarget, FontHolder& fonts, SoundPlayer& sounds, bool networked)
: mTarget(outputTarget)
, mSceneTexture()
, mWorldView(outputTarget.getDefaultView())
, mTextures() 
, mFonts(fonts)
, mSounds(sounds)
, mSceneGraph()
, mSceneLayers()
, mWorldBounds(0.f, 0.f, mWorldView.getSize().x, 5000.f)
, mSpawnPosition(mWorldView.getSize().x / 2.f, mWorldBounds.height - mWorldView.getSize().y / 2.f)
, mScrollSpeed(-50.f)
, mScrollSpeedCompensation(1.f)
, mPlayerBats()
, mEnemySpawnPoints()
, mActiveEnemies()
, mNetworkedWorld(networked)
, mNetworkNode(nullptr)
, mFinishSprite(nullptr)
{
	mSceneTexture.create(mTarget.getSize().x, mTarget.getSize().y);

	loadTextures();
	buildScene();

	// Prepare the view
	mWorldView.setCenter(mSpawnPosition);
}

void World::setWorldScrollCompensation(float compensation)
{
	mScrollSpeedCompensation = compensation;
}

void World::update(sf::Time dt)
{
	// Scroll the world, reset player velocity
	mWorldView.move(0.f, mScrollSpeed * dt.asSeconds() * mScrollSpeedCompensation);	

	FOREACH(PlayerBat* a, mPlayerBats)
		a->setVelocity(0.f, 0.f);

	// Setup commands to destroy entities, and guide missiles
	destroyEntitiesOutsideView();
	guideMissiles();

	// Forward commands to scene graph, adapt velocity (scrolling, diagonal correction)
	while (!mCommandQueue.isEmpty())
		mSceneGraph.onCommand(mCommandQueue.pop(), dt);

	adaptPlayerVelocity();

	// Collision detection and response (may destroy entities)
	handleCollisions();

	// Remove PlayerBats that were destroyed (World::removeWrecks() only destroys the entities, not the pointers in mPlayerBat)
	auto firstToRemove = std::remove_if(mPlayerBats.begin(), mPlayerBats.end(), std::mem_fn(&PlayerBat::isMarkedForRemoval));
	mPlayerBats.erase(firstToRemove, mPlayerBats.end());

	// Remove all destroyed entities, create new ones
	mSceneGraph.removeWrecks();
	spawnEnemies();

	// Regular update step, adapt position (correct if outside view)
	mSceneGraph.update(dt, mCommandQueue);
	adaptPlayerPosition();

	updateSounds();
}

void World::draw()
{
	mTarget.setView(mWorldView);
	mTarget.draw(mSceneGraph);
}

CommandQueue& World::getCommandQueue()
{
	return mCommandQueue;
}

PlayerBat* World::getPlayerBat(int identifier) const
{
	FOREACH(PlayerBat* a, mPlayerBats)
	{
		if (a->getIdentifier() == identifier)
			return a;
	}

	return nullptr;
}

void World::removePlayerBat(int identifier)
{
	PlayerBat* PlayerBat = getPlayerBat(identifier);
	if (PlayerBat)
	{
		PlayerBat->destroy();
		mPlayerBats.erase(std::find(mPlayerBats.begin(), mPlayerBats.end(), PlayerBat));
	}
}

PlayerBat* World::addPlayerBat(int identifier)
{
	if (mPlayerBats.size() == 0)
	{
		std::unique_ptr<PlayerBat> player(new PlayerBat(PlayerBat::Player1, mTextures, mFonts));
		player->setPosition(mWorldView.getCenter());
		player->setIdentifier(identifier);

		mPlayerBats.push_back(player.get());
		mSceneLayers[UpperAir]->attachChild(std::move(player));
	}
	else
	{
		std::unique_ptr<PlayerBat> player(new PlayerBat(PlayerBat::Player2, mTextures, mFonts));
		player->setPosition(mWorldView.getCenter());
		player->setIdentifier(identifier);

		mPlayerBats.push_back(player.get());
		mSceneLayers[UpperAir]->attachChild(std::move(player));
	}

	return mPlayerBats.back();
}

void World::createPickup(sf::Vector2f position, Pickup::Type type)
{	
	std::unique_ptr<Pickup> pickup(new Pickup(type, mTextures));
	pickup->setPosition(position);
	pickup->setVelocity(0.f, 1.f);
	mSceneLayers[UpperAir]->attachChild(std::move(pickup));
}

bool World::pollGameAction(GameActions::Action& out)
{
	return mNetworkNode->pollGameAction(out);
}

void World::setCurrentBattleFieldPosition(float lineY)
{
	mWorldView.setCenter(mWorldView.getCenter().x, lineY - mWorldView.getSize().y/2);
	mSpawnPosition.y = mWorldBounds.height; 
}

void World::setWorldHeight(float height)
{
	mWorldBounds.height = height;
}

bool World::hasAlivePlayer() const
{
	return mPlayerBats.size() > 0;
}

bool World::hasPlayerReachedEnd() const
{
	if (PlayerBat* PlayerBat = getPlayerBat(1))
		return !mWorldBounds.contains(PlayerBat->getPosition());
	else 
		return false;
}

void World::loadTextures()
{
	mTextures.load(Textures::Entities, "Media/Textures/Entities.png");
	mTextures.load(Textures::Jungle, "Media/Textures/Jungle.png");
	mTextures.load(Textures::Explosion, "Media/Textures/Explosion.png");
	mTextures.load(Textures::FinishLine, "Media/Textures/FinishLine.png");
	mTextures.load(Textures::Player1, "Media/Textures/player1.png");
	mTextures.load(Textures::Player2, "Media/Textures/player2.png");
}

void World::adaptPlayerPosition()
{
	// Keep player's position inside the screen bounds, at least borderDistance units from the border
	sf::FloatRect viewBounds = getViewBounds();
	const float borderDistance = 40.f;

	FOREACH(PlayerBat* PlayerBat, mPlayerBats)
	{
		sf::Vector2f position = PlayerBat->getPosition();
		position.x = std::max(position.x, viewBounds.left + borderDistance);
		position.x = std::min(position.x, viewBounds.left + viewBounds.width - borderDistance);
		position.y = std::max(position.y, viewBounds.top + borderDistance);
		position.y = std::min(position.y, viewBounds.top + viewBounds.height - borderDistance);
		PlayerBat->setPosition(position);
	}
}

void World::adaptPlayerVelocity()
{
	FOREACH(PlayerBat* PlayerBat, mPlayerBats)
	{
		sf::Vector2f velocity = PlayerBat->getVelocity();

		// If moving diagonally, reduce velocity (to have always same velocity)
		if (velocity.x != 0.f && velocity.y != 0.f)
			PlayerBat->setVelocity(velocity / std::sqrt(2.f));

		// Add scrolling velocity
		PlayerBat->accelerate(0.f, mScrollSpeed);
	}
}

bool matchesCategories(SceneNode::Pair& colliders, Category::Type type1, Category::Type type2)
{
	unsigned int category1 = colliders.first->getCategory();
	unsigned int category2 = colliders.second->getCategory();

	// Make sure first pair entry has category type1 and second has type2
	if (type1 & category1 && type2 & category2)
	{
		return true;
	}
	else if (type1 & category2 && type2 & category1)
	{
		std::swap(colliders.first, colliders.second);
		return true;
	}
	else
	{
		return false;
	}
}

void World::handleCollisions()
{
	std::set<SceneNode::Pair> collisionPairs;
	mSceneGraph.checkSceneCollision(mSceneGraph, collisionPairs);

	FOREACH(SceneNode::Pair pair, collisionPairs)
	{
		if (matchesCategories(pair, Category::PlayerBat, Category::EnemyBat))
		{
			auto& player = static_cast<PlayerBat&>(*pair.first);
			auto& enemy = static_cast<PlayerBat&>(*pair.second);

			// Collision: Player damage = enemy's remaining HP
			player.damage(enemy.getHitpoints());
			enemy.destroy();
		}

		else if (matchesCategories(pair, Category::PlayerBat, Category::Pickup))
		{
			auto& player = static_cast<PlayerBat&>(*pair.first);
			auto& pickup = static_cast<Pickup&>(*pair.second);

			// Apply pickup effect to player, destroy projectile
			pickup.apply(player);
			pickup.destroy();
			player.playLocalSound(mCommandQueue, SoundEffect::CollectPickup);
		}
	}
}

void World::updateSounds()
{
	sf::Vector2f listenerPosition;

	// 0 players (multiplayer mode, until server is connected) -> view center
	if (mPlayerBats.empty())
	{
		listenerPosition = mWorldView.getCenter();
	}

	// 1 or more players -> mean position between all PlayerBats
	else
	{
		FOREACH(PlayerBat* PlayerBat, mPlayerBats)
			listenerPosition += PlayerBat->getWorldPosition();

		listenerPosition /= static_cast<float>(mPlayerBats.size());
	}

	// Set listener's position
	mSounds.setListenerPosition(listenerPosition);

	// Remove unused sounds
	mSounds.removeStoppedSounds();
}

void World::buildScene()
{
	// Initialize the different layers
	for (std::size_t i = 0; i < LayerCount; ++i)
	{
		Category::Type category = (i == LowerAir) ? Category::SceneAirLayer : Category::None;

		SceneNode::Ptr layer(new SceneNode(category));
		mSceneLayers[i] = layer.get();

		mSceneGraph.attachChild(std::move(layer));
	}

	// Prepare the tiled background
	sf::Texture& jungleTexture = mTextures.get(Textures::Jungle);
	jungleTexture.setRepeated(true);

	float viewHeight = mWorldView.getSize().y;
	sf::IntRect textureRect(mWorldBounds);
	textureRect.height += static_cast<int>(viewHeight);

	// Add the background sprite to the scene
	std::unique_ptr<SpriteNode> jungleSprite(new SpriteNode(jungleTexture, textureRect));
	jungleSprite->setPosition(mWorldBounds.left, mWorldBounds.top - viewHeight);
	mSceneLayers[Background]->attachChild(std::move(jungleSprite));

	// Add the finish line to the scene
	sf::Texture& finishTexture = mTextures.get(Textures::FinishLine);
	std::unique_ptr<SpriteNode> finishSprite(new SpriteNode(finishTexture));
	finishSprite->setPosition(0.f, -76.f);
	mFinishSprite = finishSprite.get();
	mSceneLayers[Background]->attachChild(std::move(finishSprite));

	// Add sound effect node
	std::unique_ptr<SoundNode> soundNode(new SoundNode(mSounds));
	mSceneGraph.attachChild(std::move(soundNode));

	// Add network node, if necessary
	if (mNetworkedWorld)
	{
		std::unique_ptr<NetworkNode> networkNode(new NetworkNode());
		mNetworkNode = networkNode.get();
		mSceneGraph.attachChild(std::move(networkNode));
	}

	// Add enemy PlayerBat
	addEnemies();
}

void World::addEnemies()
{
	if (mNetworkedWorld)
		return;

	// Add enemies to the spawn point container
	addEnemy(PlayerBat::Raptor,    0.f,  500.f);
	addEnemy(PlayerBat::Raptor,    0.f, 1000.f);
	addEnemy(PlayerBat::Raptor, +100.f, 1150.f);
	addEnemy(PlayerBat::Raptor, -100.f, 1150.f);
	addEnemy(PlayerBat::Avenger,  70.f, 1500.f);
	addEnemy(PlayerBat::Avenger, -70.f, 1500.f);
	addEnemy(PlayerBat::Avenger, -70.f, 1710.f);
	addEnemy(PlayerBat::Avenger,  70.f, 1700.f);
	addEnemy(PlayerBat::Avenger,  30.f, 1850.f);
	addEnemy(PlayerBat::Raptor,  300.f, 2200.f);
	addEnemy(PlayerBat::Raptor, -300.f, 2200.f);
	addEnemy(PlayerBat::Raptor,    0.f, 2200.f);
	addEnemy(PlayerBat::Raptor,    0.f, 2500.f);
	addEnemy(PlayerBat::Avenger,-300.f, 2700.f);
	addEnemy(PlayerBat::Avenger,-300.f, 2700.f);
	addEnemy(PlayerBat::Raptor,    0.f, 3000.f);
	addEnemy(PlayerBat::Raptor,  250.f, 3250.f);
	addEnemy(PlayerBat::Raptor, -250.f, 3250.f);
	addEnemy(PlayerBat::Avenger,   0.f, 3500.f);
	addEnemy(PlayerBat::Avenger,   0.f, 3700.f);
	addEnemy(PlayerBat::Raptor,    0.f, 3800.f);
	addEnemy(PlayerBat::Avenger,   0.f, 4000.f);
	addEnemy(PlayerBat::Avenger,-200.f, 4200.f);
	addEnemy(PlayerBat::Raptor,  200.f, 4200.f);
	addEnemy(PlayerBat::Raptor,    0.f, 4400.f);

	sortEnemies();
}

void World::sortEnemies()
{
	// Sort all enemies according to their y value, such that lower enemies are checked first for spawning
	std::sort(mEnemySpawnPoints.begin(), mEnemySpawnPoints.end(), [] (SpawnPoint lhs, SpawnPoint rhs)
	{
		return lhs.y < rhs.y;
	});
}

void World::addEnemy(PlayerBat::Type type, float relX, float relY)
{
	SpawnPoint spawn(type, mSpawnPosition.x + relX, mSpawnPosition.y - relY);
	mEnemySpawnPoints.push_back(spawn);
}

void World::spawnEnemies()
{
	// Spawn all enemies entering the view area (including distance) this frame
	while (!mEnemySpawnPoints.empty()
		&& mEnemySpawnPoints.back().y > getBattlefieldBounds().top)
	{
		SpawnPoint spawn = mEnemySpawnPoints.back();
		
		std::unique_ptr<PlayerBat> enemy(new PlayerBat(spawn.type, mTextures, mFonts));
		enemy->setPosition(spawn.x, spawn.y);
		enemy->setRotation(180.f);
		if (mNetworkedWorld) enemy->disablePickups();

		mSceneLayers[UpperAir]->attachChild(std::move(enemy));

		// Enemy is spawned, remove from the list to spawn
		mEnemySpawnPoints.pop_back();
	}
}

void World::destroyEntitiesOutsideView()
{
	Command command;
	command.category = Category::Projectile | Category::EnemyBat;
	command.action = derivedAction<Entity>([this] (Entity& e, sf::Time)
	{
		if (!getBattlefieldBounds().intersects(e.getBoundingRect()))
			e.remove();
	});

	mCommandQueue.push(command);
}

void World::guideMissiles()
{
	// Setup command that stores all enemies in mActiveEnemies
	Command enemyCollector;
	enemyCollector.category = Category::EnemyBat;
	enemyCollector.action = derivedAction<PlayerBat>([this] (PlayerBat& enemy, sf::Time)
	{
		if (!enemy.isDestroyed())
			mActiveEnemies.push_back(&enemy);
	});

	// Setup command that guides all missiles to the enemy which is currently closest to the player
	Command missileGuider;
	missileGuider.category = Category::AlliedProjectile;
	missileGuider.action = derivedAction<Projectile>([this] (Projectile& missile, sf::Time)
	{
		float minDistance = std::numeric_limits<float>::max();
		PlayerBat* closestEnemy = nullptr;

		// Find closest enemy
		FOREACH(PlayerBat* enemy, mActiveEnemies)
		{
			float enemyDistance = distance(missile, *enemy);

			if (enemyDistance < minDistance)
			{
				closestEnemy = enemy;
				minDistance = enemyDistance;
			}
		}

		if (closestEnemy)
			missile.guideTowards(closestEnemy->getWorldPosition());
	});

	// Push commands, reset active enemies
	mCommandQueue.push(enemyCollector);
	mCommandQueue.push(missileGuider);
	mActiveEnemies.clear();
}

void World::addGoals()
{
	//Error
	//Goal goal(0, 5, 100, 100);
	//Goal goal2(1, 5, 100, 500);
	//mGoals.push_back(goal);
	//mGoals.push_back(goal2);
}

sf::FloatRect World::getViewBounds() const
{
	return sf::FloatRect(mWorldView.getCenter() - mWorldView.getSize() / 2.f, mWorldView.getSize());
}

sf::FloatRect World::getBattlefieldBounds() const
{
	// Return view bounds + some area at top, where enemies spawn
	sf::FloatRect bounds = getViewBounds();
	bounds.top -= 100.f;
	bounds.height += 100.f;

	return bounds;
}
