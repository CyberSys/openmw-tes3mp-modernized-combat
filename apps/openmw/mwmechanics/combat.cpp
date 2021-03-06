#include "combat.hpp"

#include <components/misc/rng.hpp>
#include <components/settings/settings.hpp>

#include <components/sceneutil/positionattitudetransform.hpp>

/*
    Start of tes3mp addition

    Include additional headers for multiplayer purposes
*/
#include <components/openmw-mp/MWMPLog.hpp>
#include "../mwmp/Main.hpp"
#include "../mwmp/LocalPlayer.hpp"
#include "../mwmp/PlayerList.hpp"
#include "../mwmp/CellController.hpp"
#include "../mwmp/MechanicsHelper.hpp"
/*
    End of tes3mp addition
*/

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/esmstore.hpp"

#include "npcstats.hpp"
#include "movement.hpp"
#include "spellcasting.hpp"
#include "difficultyscaling.hpp"
#include "actorutil.hpp"

namespace
{

float signedAngleRadians (const osg::Vec3f& v1, const osg::Vec3f& v2, const osg::Vec3f& normal)
{
    return std::atan2((normal * (v1 ^ v2)), (v1 * v2));
}

}

namespace MWMechanics
{

    bool applyOnStrikeEnchantment(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim, const MWWorld::Ptr& object, const osg::Vec3f& hitPosition, const bool fromProjectile)
    {
        std::string enchantmentName = !object.isEmpty() ? object.getClass().getEnchantment(object) : "";
        if (!enchantmentName.empty())
        {
            const ESM::Enchantment* enchantment = MWBase::Environment::get().getWorld()->getStore().get<ESM::Enchantment>().find(
                        enchantmentName);
            if (enchantment->mData.mType == ESM::Enchantment::WhenStrikes)
            {
                MWMechanics::CastSpell cast(attacker, victim, fromProjectile);
                cast.mHitPosition = hitPosition;
                cast.cast(object, false);
                return true;
            }
        }
        return false;
    }

    bool blockMeleeAttack(const MWWorld::Ptr &attacker, const MWWorld::Ptr &blocker, const MWWorld::Ptr &weapon, float damage, float attackStrength)
    {
        if (!blocker.getClass().hasInventoryStore(blocker))
            return false;

        MWMechanics::CreatureStats& blockerStats = blocker.getClass().getCreatureStats(blocker);

        if (blockerStats.getKnockedDown() // Used for both knockout or knockdown
                || blockerStats.getHitRecovery()
                || blockerStats.isParalyzed())
            return false;

        if (!MWBase::Environment::get().getMechanicsManager()->isReadyToBlock(blocker))
            return false;

        MWWorld::InventoryStore& inv = blocker.getClass().getInventoryStore(blocker);
        MWWorld::ContainerStoreIterator shield = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
        if (shield == inv.end() || shield->getTypeName() != typeid(ESM::Armor).name())
            return false;

        if (!blocker.getRefData().getBaseNode())
            return false; // shouldn't happen

        float angleDegrees = osg::RadiansToDegrees(
                    signedAngleRadians (
                    (attacker.getRefData().getPosition().asVec3() - blocker.getRefData().getPosition().asVec3()),
                    blocker.getRefData().getBaseNode()->getAttitude() * osg::Vec3f(0,1,0),
                    osg::Vec3f(0,0,1)));

        const MWWorld::Store<ESM::GameSetting>& gmst = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>();
        if (angleDegrees < gmst.find("fCombatBlockLeftAngle")->getFloat())
            return false;
        if (angleDegrees > gmst.find("fCombatBlockRightAngle")->getFloat())
            return false;

        MWMechanics::CreatureStats& attackerStats = attacker.getClass().getCreatureStats(attacker);

        float blockTerm = blocker.getClass().getSkill(blocker, ESM::Skill::Block) + 0.2f * blockerStats.getAttribute(ESM::Attribute::Agility).getModified()
            + 0.1f * blockerStats.getAttribute(ESM::Attribute::Luck).getModified();
        float enemySwing = attackStrength;
        float swingTerm = enemySwing * gmst.find("fSwingBlockMult")->getFloat() + gmst.find("fSwingBlockBase")->getFloat();

        float blockerTerm = blockTerm * swingTerm;
        if (blocker.getClass().getMovementSettings(blocker).mPosition[1] <= 0)
            blockerTerm *= gmst.find("fBlockStillBonus")->getFloat();
        blockerTerm *= blockerStats.getFatigueTerm();

        int attackerSkill = 0;
        if (weapon.isEmpty())
            attackerSkill = attacker.getClass().getSkill(attacker, ESM::Skill::HandToHand);
        else
            attackerSkill = attacker.getClass().getSkill(attacker, weapon.getClass().getEquipmentSkill(weapon));
        float attackerTerm = attackerSkill + 0.2f * attackerStats.getAttribute(ESM::Attribute::Agility).getModified()
                + 0.1f * attackerStats.getAttribute(ESM::Attribute::Luck).getModified();
        attackerTerm *= attackerStats.getFatigueTerm();

        int x = int(blockerTerm - attackerTerm);
        int iBlockMaxChance = gmst.find("iBlockMaxChance")->getInt();
        int iBlockMinChance = gmst.find("iBlockMinChance")->getInt();
        x = std::min(iBlockMaxChance, std::max(iBlockMinChance, x));

        /*
            Start of tes3mp change (major)

            Only calculate block chance for LocalPlayers and LocalActors; otherwise,
            get the block state from the relevant DedicatedPlayer or DedicatedActor
        */
        mwmp::Attack *localAttack = MechanicsHelper::getLocalAttack(attacker);

        if (localAttack)
        {
            localAttack->block = false;
        }

        mwmp::Attack *dedicatedAttack = MechanicsHelper::getDedicatedAttack(blocker);

        if ((dedicatedAttack && dedicatedAttack->block == true) ||
            Misc::Rng::roll0to99() < x)
        {
            if (localAttack)
            {
                localAttack->block = true;
            }
        /*
            End of tes3mp change (major)
        */

            if (!(weapon.isEmpty() && !attacker.getClass().isNpc())) // Unarmed creature attacks don't affect armor condition
            {
                // Reduce shield durability by incoming damage
                int shieldhealth = shield->getClass().getItemHealth(*shield);

                shieldhealth -= std::min(shieldhealth, int(damage));
                shield->getCellRef().setCharge(shieldhealth);
                if (shieldhealth == 0)
                    inv.unequipItem(*shield, blocker);
            }
            // Reduce blocker fatigue
            const float fFatigueBlockBase = gmst.find("fFatigueBlockBase")->getFloat();
            const float fFatigueBlockMult = gmst.find("fFatigueBlockMult")->getFloat();
            const float fWeaponFatigueBlockMult = gmst.find("fWeaponFatigueBlockMult")->getFloat();
            MWMechanics::DynamicStat<float> fatigue = blockerStats.getFatigue();
            float normalizedEncumbrance = blocker.getClass().getNormalizedEncumbrance(blocker);
            normalizedEncumbrance = std::min(1.f, normalizedEncumbrance);
            float fatigueLoss = fFatigueBlockBase + normalizedEncumbrance * fFatigueBlockMult;
            if (!weapon.isEmpty())
                fatigueLoss += weapon.getClass().getWeight(weapon) * attackStrength * fWeaponFatigueBlockMult;
            fatigue.setCurrent(fatigue.getCurrent() - fatigueLoss);
            blockerStats.setFatigue(fatigue);

            blockerStats.setBlock(true);

            if (blocker == getPlayer())
                blocker.getClass().skillUsageSucceeded(blocker, ESM::Skill::Block, 0);

            return true;
        }
        return false;
    }

    /* nox7 addition */
    /*
        Add method to manually block attack if player is manually blocking, this will affect damage
    */
    void attemptToManuallyBlockMeleeAttack(const MWWorld::Ptr &attacker, const MWWorld::Ptr &blocker, const MWWorld::Ptr &weapon, float &damage, float attackStrength)
    {
        if (!blocker.getClass().hasInventoryStore(blocker))
            return;

        MWMechanics::CreatureStats& blockerStats = blocker.getClass().getCreatureStats(blocker);

        if (blockerStats.getKnockedDown() // Used for both knockout or knockdown
            || blockerStats.getHitRecovery()
            || blockerStats.isParalyzed())
            return;

        if (!MWBase::Environment::get().getMechanicsManager()->isReadyToBlock(blocker))
            return;

        if (blocker == MWMechanics::getPlayer() && blockerStats.getManualBlock()) {
            // Victim is manually blocking

            // Reduce shield durability by incoming damage, before modification
            // Get the shield
            MWWorld::InventoryStore& inv = blocker.getClass().getInventoryStore(blocker);
            MWWorld::ContainerStoreIterator shield = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
            if (shield == inv.end() || shield->getTypeName() != typeid(ESM::Armor).name()) {
                // Shouldn't happen, can't block without a shield
            }
            else {
                int shieldhealth = shield->getClass().getItemHealth(*shield);

                shieldhealth -= std::min(shieldhealth, int(damage));
                shield->getCellRef().setCharge(shieldhealth);

                // If the shield broke, then unequip it and stop blocking animation pose
                if (shieldhealth == 0) {
                    blockerStats.setManualBlock(false);
                    inv.unequipItem(*shield, blocker);
                }

                // Fetch victim's block skill and apply it to reduce incoming damage
                float blockSkill = static_cast<float>(blocker.getClass().getSkill(blocker, ESM::Skill::Block));
                float blockedDamage = damage * (blockSkill / 100.0f);
                damage -= blockedDamage;
                damage = ceil(damage);
                if (damage < 0.01f) {
                    damage = 0;
                }

                // Plays the block sound for the armor piece
                blocker.getClass().block(blocker);

                // Drain the victim's fatigue from blocking
                const MWWorld::Store<ESM::GameSetting>& gmst = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>();
                const float fFatigueBlockBase = gmst.find("fFatigueBlockBase")->mValue.getFloat();
                const float fFatigueBlockMult = gmst.find("fFatigueBlockMult")->mValue.getFloat();
                const float fWeaponFatigueBlockMult = gmst.find("fWeaponFatigueBlockMult")->mValue.getFloat();
                MWMechanics::DynamicStat<float> fatigue = blockerStats.getFatigue();
                float normalizedEncumbrance = blocker.getClass().getNormalizedEncumbrance(blocker);
                normalizedEncumbrance = std::min(1.f, normalizedEncumbrance);
                float fatigueLoss = fFatigueBlockBase + normalizedEncumbrance * fFatigueBlockMult;
                if (!weapon.isEmpty())
                    fatigueLoss += weapon.getClass().getWeight(weapon) * attackStrength * fWeaponFatigueBlockMult;

                float newFatigue = fatigue.getCurrent() - fatigueLoss;
                fatigue.setCurrent(newFatigue);
                blockerStats.setFatigue(fatigue);

                // If the fatigue is too low, then they cannot block
                if (newFatigue <= 0)
                    blockerStats.setManualBlock(false);

                // Give the blocker's Block skill some experience
                if (blocker == MWMechanics::getPlayer())
                    blocker.getClass().skillUsageSucceeded(blocker, ESM::Skill::Block, 0);
            }
        }
    }

    void resistNormalWeapon(const MWWorld::Ptr &actor, const MWWorld::Ptr& attacker, const MWWorld::Ptr &weapon, float &damage)
    {
        const MWMechanics::MagicEffects& effects = actor.getClass().getCreatureStats(actor).getMagicEffects();
        float resistance = std::min(100.f, effects.get(ESM::MagicEffect::ResistNormalWeapons).getMagnitude()
                - effects.get(ESM::MagicEffect::WeaknessToNormalWeapons).getMagnitude());

        float multiplier = 1.f - resistance / 100.f;

        if (!(weapon.get<ESM::Weapon>()->mBase->mData.mFlags & ESM::Weapon::Silver
              || weapon.get<ESM::Weapon>()->mBase->mData.mFlags & ESM::Weapon::Magical))
        {
            if (weapon.getClass().getEnchantment(weapon).empty()
              || !Settings::Manager::getBool("enchanted weapons are magical", "Game"))
                damage *= multiplier;
        }

        if ((weapon.get<ESM::Weapon>()->mBase->mData.mFlags & ESM::Weapon::Silver)
                && actor.getClass().isNpc() && actor.getClass().getNpcStats(actor).isWerewolf())
            damage *= MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>().find("fWereWolfSilverWeaponDamageMult")->getFloat();

        if (damage == 0 && attacker == getPlayer())
            MWBase::Environment::get().getWindowManager()->messageBox("#{sMagicTargetResistsWeapons}");
    }

    void projectileHit(const MWWorld::Ptr& attacker, const MWWorld::Ptr& victim, MWWorld::Ptr weapon, const MWWorld::Ptr& projectile,
                       const osg::Vec3f& hitPosition, float attackStrength)
    {
        /*
            Start of tes3mp addition

            Ignore projectiles fired by DedicatedPlayers and DedicatedActors

            If fired by LocalPlayers and LocalActors, get the associated LocalAttack and set its type
            to RANGED while also marking it as a hit
        */
        if (mwmp::PlayerList::isDedicatedPlayer(attacker) || mwmp::Main::get().getCellController()->isDedicatedActor(attacker))
            return;

        mwmp::Attack *localAttack = MechanicsHelper::getLocalAttack(attacker);

        if (localAttack)
        {
            localAttack->type = mwmp::Attack::RANGED;
            localAttack->isHit = true;
        }
        /*
            End of tes3mp addition
        */

        MWBase::World *world = MWBase::Environment::get().getWorld();
        const MWWorld::Store<ESM::GameSetting> &gmst = world->getStore().get<ESM::GameSetting>();

        bool validVictim = !victim.isEmpty() && victim.getClass().isActor();

        float damage = 0.f;
        if (validVictim)
        {
            if (attacker == getPlayer())
                MWBase::Environment::get().getWindowManager()->setEnemy(victim);

            int weaponSkill = ESM::Skill::Marksman;
            if (!weapon.isEmpty())
                weaponSkill = weapon.getClass().getEquipmentSkill(weapon);

            int skillValue = attacker.getClass().getSkill(attacker, weapon.getClass().getEquipmentSkill(weapon));

            /*
                Start of tes3mp addition

                Mark this as a successful attack for the associated LocalAttack unless proven otherwise
            */
            if (localAttack)
                localAttack->success = true;
            /*
                End of tes3mp addition
            */

            if (Misc::Rng::roll0to99() >= getHitChance(attacker, victim, skillValue))
            {
                /*
                    Start of tes3mp addition

                    Mark this as a failed LocalAttack now that the hit roll has failed
                */
                if (localAttack)
                    localAttack->success = false;
                /*
                    End of tes3mp addition
                */

                victim.getClass().onHit(victim, damage, false, projectile, attacker, osg::Vec3f(), false);
                MWMechanics::reduceWeaponCondition(damage, false, weapon, attacker);
                return;
            }

            const unsigned char* attack = weapon.get<ESM::Weapon>()->mBase->mData.mChop;
            damage = attack[0] + ((attack[1] - attack[0]) * attackStrength); // Bow/crossbow damage

            // Arrow/bolt damage
            // NB in case of thrown weapons, we are applying the damage twice since projectile == weapon
            attack = projectile.get<ESM::Weapon>()->mBase->mData.mChop;
            damage += attack[0] + ((attack[1] - attack[0]) * attackStrength);

            adjustWeaponDamage(damage, weapon, attacker);

            if (attacker == getPlayer())
            {
                attacker.getClass().skillUsageSucceeded(attacker, weaponSkill, 0);
                const MWMechanics::AiSequence& sequence = victim.getClass().getCreatureStats(victim).getAiSequence();

                bool unaware = !sequence.isInCombat()
                    && !MWBase::Environment::get().getMechanicsManager()->awarenessCheck(attacker, victim);

                if (unaware)
                {
                    damage *= gmst.find("fCombatCriticalStrikeMult")->getFloat();
                    MWBase::Environment::get().getWindowManager()->messageBox("#{sTargetCriticalStrike}");
                    MWBase::Environment::get().getSoundManager()->playSound3D(victim, "critical damage", 1.0f, 1.0f);
                }
            }

            if (victim.getClass().getCreatureStats(victim).getKnockedDown())
                damage *= gmst.find("fCombatKODamageMult")->getFloat();
        }

        reduceWeaponCondition(damage, validVictim, weapon, attacker);

        // Apply "On hit" effect of the weapon & projectile

        /*
            Start of tes3mp change (minor)

            Track whether the strike enchantment is successful for attacks by the
            LocalPlayer or LocalActors for both their weapon and projectile
        */
        bool appliedEnchantment = applyOnStrikeEnchantment(attacker, victim, weapon, hitPosition, true);

        if (localAttack)
            localAttack->applyWeaponEnchantment = appliedEnchantment;

        if (weapon != projectile)
        {
            appliedEnchantment = applyOnStrikeEnchantment(attacker, victim, projectile, hitPosition, true);

            if (localAttack)
                localAttack->applyAmmoEnchantment = appliedEnchantment;
        }
        /*
            End of tes3mp change (minor)
        */

        if (validVictim)
        {
            // Non-enchanted arrows shot at enemies have a chance to turn up in their inventory
            if (victim != getPlayer() && !appliedEnchantment)
            {
                float fProjectileThrownStoreChance = gmst.find("fProjectileThrownStoreChance")->getFloat();
                if (Misc::Rng::rollProbability() < fProjectileThrownStoreChance / 100.f)
                    victim.getClass().getContainerStore(victim).add(projectile, 1, victim);
            }

            victim.getClass().onHit(victim, damage, true, projectile, attacker, hitPosition, true);
        }
        /*
            Start of tes3mp addition

            If this is a local attack that had no victim, send a packet for it here
        */
        else if (localAttack)
        {
            localAttack->hitPosition = MechanicsHelper::getPositionFromVector(hitPosition);
            localAttack->shouldSend = true;
        }
        /*
            End of tes3mp addition
        */
    }

    float getHitChance(const MWWorld::Ptr &attacker, const MWWorld::Ptr &victim, int skillValue)
    {
        MWMechanics::CreatureStats &stats = attacker.getClass().getCreatureStats(attacker);
        const MWMechanics::MagicEffects &mageffects = stats.getMagicEffects();

        MWBase::World *world = MWBase::Environment::get().getWorld();
        const MWWorld::Store<ESM::GameSetting> &gmst = world->getStore().get<ESM::GameSetting>();

        float defenseTerm = 0;
        MWMechanics::CreatureStats& victimStats = victim.getClass().getCreatureStats(victim);
        if (victimStats.getFatigue().getCurrent() >= 0)
        {
            // Maybe we should keep an aware state for actors updated every so often instead of testing every time
            bool unaware = (!victimStats.getAiSequence().isInCombat())
                    && (attacker == getPlayer())
                    && (!MWBase::Environment::get().getMechanicsManager()->awarenessCheck(attacker, victim));
            if (!(victimStats.getKnockedDown() ||
                    victimStats.isParalyzed()
                    || unaware ))
            {
                defenseTerm = victimStats.getEvasion();
            }
            defenseTerm += std::min(100.f,
                                    gmst.find("fCombatInvisoMult")->getFloat() *
                                    victimStats.getMagicEffects().get(ESM::MagicEffect::Chameleon).getMagnitude());
            defenseTerm += std::min(100.f,
                                    gmst.find("fCombatInvisoMult")->getFloat() *
                                    victimStats.getMagicEffects().get(ESM::MagicEffect::Invisibility).getMagnitude());
        }
        float attackTerm = skillValue +
                          (stats.getAttribute(ESM::Attribute::Agility).getModified() / 5.0f) +
                          (stats.getAttribute(ESM::Attribute::Luck).getModified() / 10.0f);
        attackTerm *= stats.getFatigueTerm();
        attackTerm += mageffects.get(ESM::MagicEffect::FortifyAttack).getMagnitude() -
                     mageffects.get(ESM::MagicEffect::Blind).getMagnitude();

        return round(attackTerm - defenseTerm);
    }

    void applyElementalShields(const MWWorld::Ptr &attacker, const MWWorld::Ptr &victim)
    {
        for (int i=0; i<3; ++i)
        {
            float magnitude = victim.getClass().getCreatureStats(victim).getMagicEffects().get(ESM::MagicEffect::FireShield+i).getMagnitude();

            if (!magnitude)
                continue;

            CreatureStats& attackerStats = attacker.getClass().getCreatureStats(attacker);
            float saveTerm = attacker.getClass().getSkill(attacker, ESM::Skill::Destruction)
                    + 0.2f * attackerStats.getAttribute(ESM::Attribute::Willpower).getModified()
                    + 0.1f * attackerStats.getAttribute(ESM::Attribute::Luck).getModified();

            float fatigueMax = attackerStats.getFatigue().getModified();
            float fatigueCurrent = attackerStats.getFatigue().getCurrent();

            float normalisedFatigue = floor(fatigueMax)==0 ? 1 : std::max (0.0f, (fatigueCurrent/fatigueMax));

            saveTerm *= 1.25f * normalisedFatigue;

            float x = std::max(0.f, saveTerm - Misc::Rng::roll0to99());

            int element = ESM::MagicEffect::FireDamage;
            if (i == 1)
                element = ESM::MagicEffect::ShockDamage;
            if (i == 2)
                element = ESM::MagicEffect::FrostDamage;

            float elementResistance = MWMechanics::getEffectResistanceAttribute(element, &attackerStats.getMagicEffects());

            x = std::min(100.f, x + elementResistance);

            static const float fElementalShieldMult = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>().find("fElementalShieldMult")->getFloat();
            x = fElementalShieldMult * magnitude * (1.f - 0.01f * x);

            // Note swapped victim and attacker, since the attacker takes the damage here.
            x = scaleDamage(x, victim, attacker);

            MWMechanics::DynamicStat<float> health = attackerStats.getHealth();
            health.setCurrent(health.getCurrent() - x);
            attackerStats.setHealth(health);
        }
    }

    void reduceWeaponCondition(float damage, bool hit, MWWorld::Ptr &weapon, const MWWorld::Ptr &attacker)
    {
        if (weapon.isEmpty())
            return;

        if (!hit)
            damage = 0.f;

        const bool weaphashealth = weapon.getClass().hasItemHealth(weapon);
        if(weaphashealth)
        {
            int weaphealth = weapon.getClass().getItemHealth(weapon);

            bool godmode = attacker == MWMechanics::getPlayer() && MWBase::Environment::get().getWorld()->getGodModeState();

            // weapon condition does not degrade when godmode is on
            if (!godmode)
            {
                const float fWeaponDamageMult = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>().find("fWeaponDamageMult")->getFloat();
                float x = std::max(1.f, fWeaponDamageMult * damage);

                weaphealth -= std::min(int(x), weaphealth);
                weapon.getCellRef().setCharge(weaphealth);
            }

            // Weapon broken? unequip it
            if (weaphealth == 0)
                weapon = *attacker.getClass().getInventoryStore(attacker).unequipItem(weapon, attacker);
        }
    }

    void adjustWeaponDamage(float &damage, const MWWorld::Ptr &weapon, const MWWorld::Ptr& attacker)
    {
        /* nox7 modification */
        /*
            The weapon damage is now only adjusted by the weapon's condition
            TODO A little bit of strength too later (10%?)
        */

        if (weapon.isEmpty())
            return;

        const bool weaphashealth = weapon.getClass().hasItemHealth(weapon);
        if (weaphashealth)
        {
            damage *= weapon.getClass().getItemNormalizedHealth(weapon);
        }

        // static const float fDamageStrengthBase = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>()
        //        .find("fDamageStrengthBase")->mValue.getFloat();
        // static const float fDamageStrengthMult = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>()
        //        .find("fDamageStrengthMult")->mValue.getFloat();
        // damage *= fDamageStrengthBase +
        //        (attacker.getClass().getCreatureStats(attacker).getAttribute(ESM::Attribute::Strength).getModified() * fDamageStrengthMult * 0.1f);
    }

    void getHandToHandDamage(const MWWorld::Ptr &attacker, const MWWorld::Ptr &victim, float &damage, bool &healthdmg, float attackStrength)
    {
        // Note: MCP contains an option to include Strength in hand-to-hand damage
        // calculations. Some mods recommend using it, so we may want to include an
        // option for it.
        const MWWorld::ESMStore& store = MWBase::Environment::get().getWorld()->getStore();
        float minstrike = store.get<ESM::GameSetting>().find("fMinHandToHandMult")->getFloat();
        float maxstrike = store.get<ESM::GameSetting>().find("fMaxHandToHandMult")->getFloat();
        damage  = static_cast<float>(attacker.getClass().getSkill(attacker, ESM::Skill::HandToHand));
        damage *= minstrike + ((maxstrike-minstrike)*attackStrength);

        MWMechanics::CreatureStats& otherstats = victim.getClass().getCreatureStats(victim);
        healthdmg = otherstats.isParalyzed()
                || otherstats.getKnockedDown();
        bool isWerewolf = (attacker.getClass().isNpc() && attacker.getClass().getNpcStats(attacker).isWerewolf());
        if(isWerewolf)
        {
            healthdmg = true;
            // GLOB instead of GMST because it gets updated during a quest
            damage *= MWBase::Environment::get().getWorld()->getGlobalFloat("werewolfclawmult");
        }
        if(healthdmg)
            damage *= store.get<ESM::GameSetting>().find("fHandtoHandHealthPer")->getFloat();

        MWBase::SoundManager *sndMgr = MWBase::Environment::get().getSoundManager();
        if(isWerewolf)
        {
            const ESM::Sound *sound = store.get<ESM::Sound>().searchRandom("WolfHit");
            if(sound)
                sndMgr->playSound3D(victim, sound->mId, 1.0f, 1.0f);
        }
        else
            sndMgr->playSound3D(victim, "Hand To Hand Hit", 1.0f, 1.0f);
    }

    void applyFatigueLoss(const MWWorld::Ptr &attacker, const MWWorld::Ptr &weapon, float attackStrength)
    {
        // somewhat of a guess, but using the weapon weight makes sense
        const MWWorld::Store<ESM::GameSetting>& store = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>();
        const float fFatigueAttackBase = store.find("fFatigueAttackBase")->getFloat();
        const float fFatigueAttackMult = store.find("fFatigueAttackMult")->getFloat();
        const float fWeaponFatigueMult = store.find("fWeaponFatigueMult")->getFloat();
        CreatureStats& stats = attacker.getClass().getCreatureStats(attacker);
        MWMechanics::DynamicStat<float> fatigue = stats.getFatigue();
        const float normalizedEncumbrance = attacker.getClass().getNormalizedEncumbrance(attacker);

        bool godmode = attacker == MWMechanics::getPlayer() && MWBase::Environment::get().getWorld()->getGodModeState();

        if (!godmode)
        {
            float fatigueLoss = fFatigueAttackBase + normalizedEncumbrance * fFatigueAttackMult;
            if (!weapon.isEmpty())
                fatigueLoss += weapon.getClass().getWeight(weapon) * attackStrength * fWeaponFatigueMult;
            fatigue.setCurrent(fatigue.getCurrent() - fatigueLoss);
            stats.setFatigue(fatigue);
        }
    }

    float getFightDistanceBias(const MWWorld::Ptr& actor1, const MWWorld::Ptr& actor2)
    {
        osg::Vec3f pos1 (actor1.getRefData().getPosition().asVec3());
        osg::Vec3f pos2 (actor2.getRefData().getPosition().asVec3());

        float d = (pos1 - pos2).length();

        static const int iFightDistanceBase = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>().find(
                    "iFightDistanceBase")->getInt();
        static const float fFightDistanceMultiplier = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>().find(
                    "fFightDistanceMultiplier")->getFloat();

        return (iFightDistanceBase - fFightDistanceMultiplier * d);
    }
}
