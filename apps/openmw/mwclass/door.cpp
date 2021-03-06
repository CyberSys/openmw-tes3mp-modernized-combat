#include "door.hpp"

/*
    Start of tes3mp addition

    Include additional headers for multiplayer purposes
*/
#include "../mwmp/Main.hpp"
#include "../mwmp/Networking.hpp"
#include "../mwmp/ObjectList.hpp"
/*
    End of tes3mp addition
*/

#include <components/esm/loaddoor.hpp>
#include <components/esm/doorstate.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"

#include "../mwworld/ptr.hpp"
#include "../mwworld/nullaction.hpp"
#include "../mwworld/failedaction.hpp"
#include "../mwworld/actionteleport.hpp"
#include "../mwworld/actiondoor.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwphysics/physicssystem.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/actiontrap.hpp"
#include "../mwworld/customdata.hpp"

#include "../mwgui/tooltips.hpp"

#include "../mwrender/objects.hpp"
#include "../mwrender/renderinginterface.hpp"
#include "../mwrender/animation.hpp"

#include "../mwmechanics/actorutil.hpp"

namespace MWClass
{
    class DoorCustomData : public MWWorld::CustomData
    {
    public:
        MWWorld::DoorState mDoorState;

        virtual MWWorld::CustomData *clone() const;

        virtual DoorCustomData& asDoorCustomData()
        {
            return *this;
        }
        virtual const DoorCustomData& asDoorCustomData() const
        {
            return *this;
        }
    };

    MWWorld::CustomData *DoorCustomData::clone() const
    {
        return new DoorCustomData (*this);
    }

    void Door::insertObjectRendering (const MWWorld::Ptr& ptr, const std::string& model, MWRender::RenderingInterface& renderingInterface) const
    {
        if (!model.empty()) {
            renderingInterface.getObjects().insertModel(ptr, model, true);
        }
    }

    void Door::insertObject(const MWWorld::Ptr& ptr, const std::string& model, MWPhysics::PhysicsSystem& physics) const
    {
        if(!model.empty())
            physics.addObject(ptr, model, MWPhysics::CollisionType_Door);

        // Resume the door's opening/closing animation if it wasn't finished
        if (ptr.getRefData().getCustomData())
        {
            const DoorCustomData& customData = ptr.getRefData().getCustomData()->asDoorCustomData();
            if (customData.mDoorState != MWWorld::DoorState::Idle)
            {
                MWBase::Environment::get().getWorld()->activateDoor(ptr, customData.mDoorState);
            }
        }
    }

    bool Door::isDoor() const
    {
        return true;
    }

    bool Door::useAnim() const
    {
        return true;
    }

    std::string Door::getModel(const MWWorld::ConstPtr &ptr) const
    {
        const MWWorld::LiveCellRef<ESM::Door> *ref = ptr.get<ESM::Door>();

        const std::string &model = ref->mBase->mModel;
        if (!model.empty()) {
            return "meshes\\" + model;
        }
        return "";
    }

    std::string Door::getName (const MWWorld::ConstPtr& ptr) const
    {
        const MWWorld::LiveCellRef<ESM::Door> *ref = ptr.get<ESM::Door>();

        return ref->mBase->mName;
    }

    std::shared_ptr<MWWorld::Action> Door::activate (const MWWorld::Ptr& ptr,
        const MWWorld::Ptr& actor) const
    {
        MWWorld::LiveCellRef<ESM::Door> *ref = ptr.get<ESM::Door>();

        const std::string &openSound = ref->mBase->mOpenSound;
        const std::string &closeSound = ref->mBase->mCloseSound;
        const std::string lockedSound = "LockedDoor";
        const std::string trapActivationSound = "Disarm Trap Fail";

        MWWorld::ContainerStore &invStore = actor.getClass().getContainerStore(actor);

        bool isLocked = ptr.getCellRef().getLockLevel() > 0;
        bool isTrapped = !ptr.getCellRef().getTrap().empty();
        bool hasKey = false;
        std::string keyName;

        // FIXME: If NPC activate teleporting door, it can lead to crash due to iterator invalidation in the Actors update.
        // Make such activation a no-op for now, like how it is in the vanilla game.
        if (actor != MWMechanics::getPlayer() && ptr.getCellRef().getTeleport())
        {
            std::shared_ptr<MWWorld::Action> action(new MWWorld::FailedAction(std::string(), ptr));
            action->setSound(lockedSound);
            return action;
        }

        // make door glow if player activates it with telekinesis
        if (actor == MWMechanics::getPlayer() &&
            MWBase::Environment::get().getWorld()->getDistanceToFacedObject() >
            MWBase::Environment::get().getWorld()->getMaxActivationDistance())
        {
            MWRender::Animation* animation = MWBase::Environment::get().getWorld()->getAnimation(ptr);

            const MWWorld::ESMStore& store = MWBase::Environment::get().getWorld()->getStore();
            int index = ESM::MagicEffect::effectStringToId("sEffectTelekinesis");
            const ESM::MagicEffect *effect = store.get<ESM::MagicEffect>().find(index);

            animation->addSpellCastGlow(effect, 1); // 1 second glow to match the time taken for a door opening or closing
        }

        const std::string keyId = ptr.getCellRef().getKey();
        if (!keyId.empty())
        {
            MWWorld::Ptr keyPtr = invStore.search(keyId);
            if (!keyPtr.isEmpty())
            {
                hasKey = true;
                keyName = keyPtr.getClass().getName(keyPtr);
            }
        }

        if ((isLocked || isTrapped) && hasKey)
        {
            if(actor == MWMechanics::getPlayer())
                MWBase::Environment::get().getWindowManager()->messageBox(keyName + " #{sKeyUsed}");

            /*
                Start of tes3mp change (major)

                Disable unilateral unlocking on this client and expect the server's reply to our
                packet to do it instead
            */
            //if(isLocked)
            //    unlock(ptr); //Call the function here. because that makes sense.
            /*
                End of tes3mp change (major)
            */

            // using a key disarms the trap
            if(isTrapped)
            {
                /*
                    Start of tes3mp change (major)

                    Disable unilateral trap disarming on this client and expect the server's reply to our
                    packet to do it instead
                */
                //ptr.getCellRef().setTrap("");
                /*
                    End of tes3mp change (major)
                */

                MWBase::Environment::get().getSoundManager()->playSound3D(ptr, "Disarm Trap", 1.0f, 1.0f);
                isTrapped = false;

                /*
                    Start of tes3mp addition

                    Send an ID_OBJECT_TRAP packet every time a trap is disarmed
                */
                mwmp::ObjectList *objectList = mwmp::Main::get().getNetworking()->getObjectList();
                objectList->reset();
                objectList->packetOrigin = mwmp::CLIENT_GAMEPLAY;
                objectList->addObjectTrap(ptr, ptr.getRefData().getPosition(), true);
                objectList->sendObjectTrap();
                /*
                    End of tes3mp addition
                */
            }

            /*
                Start of tes3mp addition

                Send an ID_OBJECT_LOCK packet every time a door is unlocked here
            */
            if (isLocked)
            {
                mwmp::ObjectList *objectList = mwmp::Main::get().getNetworking()->getObjectList();
                objectList->reset();
                objectList->packetOrigin = mwmp::CLIENT_GAMEPLAY;
                objectList->addObjectLock(ptr, 0);
                objectList->sendObjectLock();
            }
            /*
                End of tes3mp addition
            */
        }

        if (!isLocked || hasKey)
        {
            if(isTrapped)
            {
                // Trap activation
                std::shared_ptr<MWWorld::Action> action(new MWWorld::ActionTrap(ptr.getCellRef().getTrap(), ptr));
                action->setSound(trapActivationSound);
                return action;
            }

            if (ptr.getCellRef().getTeleport())
            {
                if (actor == MWMechanics::getPlayer() && MWBase::Environment::get().getWorld()->getDistanceToFacedObject() > MWBase::Environment::get().getWorld()->getMaxActivationDistance())
                {
                    // player activated teleport door with telekinesis
                    std::shared_ptr<MWWorld::Action> action(new MWWorld::FailedAction);
                    return action;
                }
                else
                {
                    std::shared_ptr<MWWorld::Action> action(new MWWorld::ActionTeleport (ptr.getCellRef().getDestCell(), ptr.getCellRef().getDoorDest(), true));
                    action->setSound(openSound);
                    return action;
                }
            }
            else
            {
                // animated door
                std::shared_ptr<MWWorld::Action> action(new MWWorld::ActionDoor(ptr));
                const auto doorState = getDoorState(ptr);
                bool opening = true;
                float doorRot = ptr.getRefData().getPosition().rot[2] - ptr.getCellRef().getPosition().rot[2];
                if (doorState == MWWorld::DoorState::Opening)
                    opening = false;
                if (doorState == MWWorld::DoorState::Idle && doorRot != 0)
                    opening = false;

                if (opening)
                {
                    MWBase::Environment::get().getSoundManager()->fadeOutSound3D(ptr,
                            closeSound, 0.5f);
                    // Doors rotate at 90 degrees per second, so start the sound at
                    // where it would be at the current rotation.
                    float offset = doorRot/(3.14159265f * 0.5f);
                    action->setSoundOffset(offset);
                    action->setSound(openSound);
                }
                else
                {
                    MWBase::Environment::get().getSoundManager()->fadeOutSound3D(ptr,
                                                openSound, 0.5f);
                    float offset = 1.0f - doorRot/(3.14159265f * 0.5f);
                    action->setSoundOffset(std::max(offset, 0.0f));
                    action->setSound(closeSound);
                }

                return action;
            }
        }
        else
        {
            // locked, and we can't open.
            std::shared_ptr<MWWorld::Action> action(new MWWorld::FailedAction(std::string(), ptr));
            action->setSound(lockedSound);
            return action;
        }
    }

    void Door::lock (const MWWorld::Ptr& ptr, int lockLevel) const
    {
        if(lockLevel != 0)
            ptr.getCellRef().setLockLevel(abs(lockLevel)); //Changes lock to locklevel, if positive
        else
            ptr.getCellRef().setLockLevel(ESM::UnbreakableLock); // If zero, set to max lock level
    }

    void Door::unlock (const MWWorld::Ptr& ptr) const
    {
        int lockLevel = ptr.getCellRef().getLockLevel();
        ptr.getCellRef().setLockLevel(-abs(lockLevel)); //Makes lockLevel negative
    }

    bool Door::canLock(const MWWorld::ConstPtr &ptr) const
    {
        return true;
    }

    bool Door::allowTelekinesis(const MWWorld::ConstPtr &ptr) const
    {
        if (ptr.getCellRef().getTeleport() && ptr.getCellRef().getLockLevel() <= 0 && ptr.getCellRef().getTrap().empty())
            return false;
        else
            return true;
    }

    std::string Door::getScript (const MWWorld::ConstPtr& ptr) const
    {
        const MWWorld::LiveCellRef<ESM::Door> *ref = ptr.get<ESM::Door>();

        return ref->mBase->mScript;
    }

    void Door::registerSelf()
    {
        std::shared_ptr<Class> instance (new Door);

        registerClass (typeid (ESM::Door).name(), instance);
    }

    bool Door::hasToolTip (const MWWorld::ConstPtr& ptr) const
    {
        const MWWorld::LiveCellRef<ESM::Door> *ref = ptr.get<ESM::Door>();

        return (ref->mBase->mName != "");
    }

    MWGui::ToolTipInfo Door::getToolTipInfo (const MWWorld::ConstPtr& ptr, int count) const
    {
        const MWWorld::LiveCellRef<ESM::Door> *ref = ptr.get<ESM::Door>();

        MWGui::ToolTipInfo info;
        info.caption = ref->mBase->mName;

        std::string text;

        if (ptr.getCellRef().getTeleport())
        {
            text += "\n#{sTo}";
            text += "\n" + getDestination(*ref);
        }

        int lockLevel = ptr.getCellRef().getLockLevel();
        if (lockLevel > 0 && lockLevel != ESM::UnbreakableLock)
            text += "\n#{sLockLevel}: " + MWGui::ToolTips::toString(ptr.getCellRef().getLockLevel());
        else if (ptr.getCellRef().getLockLevel() < 0)
            text += "\n#{sUnlocked}";
        if (ptr.getCellRef().getTrap() != "")
            text += "\n#{sTrapped}";

        if (MWBase::Environment::get().getWindowManager()->getFullHelp())
        {
            text += MWGui::ToolTips::getCellRefString(ptr.getCellRef());
            text += MWGui::ToolTips::getMiscString(ref->mBase->mScript, "Script");
        }
        info.text = text;

        return info;
    }

    std::string Door::getDestination (const MWWorld::LiveCellRef<ESM::Door>& door)
    {
        const MWWorld::ESMStore& store = MWBase::Environment::get().getWorld()->getStore();

        std::string dest;
        if (door.mRef.getDestCell() != "")
        {
            // door leads to an interior, use interior name as tooltip
            dest = door.mRef.getDestCell();
        }
        else
        {
            // door leads to exterior, use cell name (if any), otherwise translated region name
            int x,y;
            MWBase::Environment::get().getWorld()->positionToIndex (door.mRef.getDoorDest().pos[0], door.mRef.getDoorDest().pos[1], x, y);
            const ESM::Cell* cell = store.get<ESM::Cell>().find(x,y);
            if (cell->mName != "")
                dest = cell->mName;
            else
            {
                const ESM::Region* region =
                    store.get<ESM::Region>().find(cell->mRegion);

                //name as is, not a token
                return region->mName;
            }
        }

        return "#{sCell=" + dest + "}";
    }

    MWWorld::Ptr Door::copyToCellImpl(const MWWorld::ConstPtr &ptr, MWWorld::CellStore &cell) const
    {
        const MWWorld::LiveCellRef<ESM::Door> *ref = ptr.get<ESM::Door>();

        return MWWorld::Ptr(cell.insert(ref), &cell);
    }

    void Door::ensureCustomData(const MWWorld::Ptr &ptr) const
    {
        if (!ptr.getRefData().getCustomData())
        {
            std::unique_ptr<DoorCustomData> data(new DoorCustomData);

            data->mDoorState = MWWorld::DoorState::Idle;
            ptr.getRefData().setCustomData(data.release());
        }
    }

    MWWorld::DoorState Door::getDoorState (const MWWorld::ConstPtr &ptr) const
    {
        if (!ptr.getRefData().getCustomData())
            return MWWorld::DoorState::Idle;
        const DoorCustomData& customData = ptr.getRefData().getCustomData()->asDoorCustomData();
        return customData.mDoorState;
    }

    void Door::setDoorState (const MWWorld::Ptr &ptr, MWWorld::DoorState state) const
    {
        if (ptr.getCellRef().getTeleport())
            throw std::runtime_error("load doors can't be moved");

        ensureCustomData(ptr);
        DoorCustomData& customData = ptr.getRefData().getCustomData()->asDoorCustomData();
        customData.mDoorState = state;
    }

    void Door::readAdditionalState (const MWWorld::Ptr& ptr, const ESM::ObjectState& state) const
    {
        if (!state.mHasCustomState)
            return;
        ensureCustomData(ptr);
        DoorCustomData& customData = ptr.getRefData().getCustomData()->asDoorCustomData();

        const ESM::DoorState& state2 = dynamic_cast<const ESM::DoorState&>(state);
        customData.mDoorState = static_cast<MWWorld::DoorState>(state2.mDoorState);
    }

    void Door::writeAdditionalState (const MWWorld::ConstPtr& ptr, ESM::ObjectState& state) const
    {
        if (!ptr.getRefData().getCustomData())
        {
            state.mHasCustomState = false;
            return;
        }
        const DoorCustomData& customData = ptr.getRefData().getCustomData()->asDoorCustomData();

        ESM::DoorState& state2 = dynamic_cast<ESM::DoorState&>(state);
        state2.mDoorState = static_cast<int>(customData.mDoorState);
    }

}
