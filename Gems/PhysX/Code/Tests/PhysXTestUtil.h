/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#pragma once

#include "AzCore/Component/Component.h"
#include <AzFramework/Physics/Collision/CollisionEvents.h>
#include <AzFramework/Physics/Common/PhysicsSimulatedBodyEvents.h>
#include <AzFramework/Terrain/TerrainDataRequestBus.h>

namespace PhysX
{
    //! CollisionCallbacksListener listens to collision events for a particular sceneHandle and simulatedBodyHandle
    class CollisionCallbacksListener
    {
    public:
        explicit CollisionCallbacksListener(AZ::EntityId entityId);
        CollisionCallbacksListener(AzPhysics::SceneHandle sceneHandle, AzPhysics::SimulatedBodyHandle bodyHandle);
        ~CollisionCallbacksListener();

        AZStd::vector<AzPhysics::CollisionEvent> m_beginCollisions;
        AZStd::vector<AzPhysics::CollisionEvent> m_persistCollisions;
        AZStd::vector<AzPhysics::CollisionEvent> m_endCollisions;

        AZStd::function<void(const AzPhysics::CollisionEvent& collisionEvent)> m_onCollisionBegin;
        AZStd::function<void(const AzPhysics::CollisionEvent& collisionEvent)> m_onCollisionPersist;
        AZStd::function<void(const AzPhysics::CollisionEvent& collisionEvent)> m_onCollisionEnd;

    private:
        void InitCollisionHandlers();

        AzPhysics::SimulatedBodyEvents::OnCollisionBegin::Handler m_onCollisionBeginHandler;
        AzPhysics::SimulatedBodyEvents::OnCollisionPersist::Handler m_onCollisionPersistHandler;
        AzPhysics::SimulatedBodyEvents::OnCollisionEnd::Handler m_onCollisionEndHandler;
    };

    //! TestTriggerAreaNotificationListener listens to trigger events for a particular sceneHandle and simulatedBodyHandle
    class TestTriggerAreaNotificationListener
    {
    public:
        explicit TestTriggerAreaNotificationListener(AZ::EntityId entityId);
        TestTriggerAreaNotificationListener(AzPhysics::SceneHandle sceneHandle, AzPhysics::SimulatedBodyHandle bodyHandle);
        ~TestTriggerAreaNotificationListener();

        const AZStd::vector<AzPhysics::TriggerEvent>& GetEnteredEvents() const { return m_enteredEvents; }
        const AZStd::vector<AzPhysics::TriggerEvent>& GetExitedEvents() const { return m_exitedEvents; }

        AZStd::function<void(const AzPhysics::TriggerEvent& event)> m_onTriggerEnter;
        AZStd::function<void(const AzPhysics::TriggerEvent& event)> m_onTriggerExit;

    private:
        void InitTriggerHandlers();

        AZStd::vector<AzPhysics::TriggerEvent> m_enteredEvents;
        AZStd::vector<AzPhysics::TriggerEvent> m_exitedEvents;

        AzPhysics::SimulatedBodyEvents::OnTriggerEnter::Handler m_onTriggerEnterHandler;
        AzPhysics::SimulatedBodyEvents::OnTriggerExit::Handler m_onTriggerExitHandler;
    };


    //! Dummy component emulating presence of terrain by connecting to TerrainDataRequestBus
    //! PhysX Terrain Component skips activation if there's no terrain present,
    //! so in order to test it we also add the DummyTestTerrainComponent.
    class DummyTestTerrainComponent
        : public AZ::Component
        , private AzFramework::Terrain::TerrainDataRequestBus::Handler
    {
    public:
        AZ_COMPONENT(DummyTestTerrainComponent, "{EE4ECA23-9C27-4D5D-9C6F-271A19C0333E}");
        static void GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided)
        {
            provided.push_back(AZ_CRC_CE("TerrainService"));
        }

    private:
        ////////////////////////////////////////////////////////////////////////
        // AZ::Component interface implementation
        void Activate() override
        {
            AzFramework::Terrain::TerrainDataRequestBus::Handler::BusConnect();
        }
        void Deactivate() override
        {
            AzFramework::Terrain::TerrainDataRequestBus::Handler::BusDisconnect();
        }
        ////////////////////////////////////////////////////////////////////////

        ////////////////////////////////////////////////////////////////////////
        // TerrainDataRequestBus interface dummy implementation
        AZ::Vector2 GetTerrainHeightQueryResolution() const override
        {
            return {};
        }
        void SetTerrainHeightQueryResolution([[maybe_unused]] AZ::Vector2 queryResolution) override
        {
        }

        AZ::Aabb GetTerrainAabb() const override
        {
            return {};
        }
        void SetTerrainAabb([[maybe_unused]] const AZ::Aabb& worldBounds) override
        {
        }

        float GetHeight(AZ::Vector3, Sampler, bool*) const override
        {
            return {};
        }
        float GetHeightFromFloats(float, float, Sampler, bool*) const override { return {}; }
        AzFramework::SurfaceData::SurfaceTagWeight GetMaxSurfaceWeight(AZ::Vector3, Sampler, bool*) const override { return {}; }
        AzFramework::SurfaceData::SurfaceTagWeight GetMaxSurfaceWeightFromFloats(float, float, Sampler, bool*) const override { return {}; }
        const char* GetMaxSurfaceName(AZ::Vector3, Sampler, bool*) const override { return {}; }
        bool GetIsHoleFromFloats(float, float, Sampler) const override { return {}; }
        AZ::Vector3 GetNormal(AZ::Vector3, Sampler, bool*) const override { return {}; }
        AZ::Vector3 GetNormalFromFloats(float, float, Sampler, bool*) const override { return {}; }
        ////////////////////////////////////////////////////////////////////////
    };

} // namespace PhysX
