/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <TerrainSystem/TerrainSystem.h>
#include <AzCore/std/parallel/shared_mutex.h>
#include <SurfaceData/SurfaceDataTypes.h>
#include <SurfaceData/SurfaceDataSystemRequestBus.h>
#include <LmbrCentral/Shape/ShapeComponentBus.h>

#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/FeatureProcessorFactory.h>
#include <TerrainRenderer/TerrainFeatureProcessor.h>

using namespace Terrain;

bool TerrainLayerPriorityComparator::operator()(const AZ::EntityId& layer1id, const AZ::EntityId& layer2id) const
{
    // Comparator for insertion/keylookup.
    // Sorts into layer/priority order, highest priority first.
    AZ::u32 priority1, layer1;
    Terrain::TerrainSpawnerRequestBus::Event(layer1id, &Terrain::TerrainSpawnerRequestBus::Events::GetPriority, layer1, priority1);

    AZ::u32 priority2, layer2;
    Terrain::TerrainSpawnerRequestBus::Event(layer2id, &Terrain::TerrainSpawnerRequestBus::Events::GetPriority, layer2, priority2);

    if (layer1 < layer2)
    {
        return false;
    }
    else if (layer1 > layer2)
    {
        return true;
    }

    if (priority1 != priority2)
    {
        return priority1 > priority2;
    }

    return layer1id > layer2id;
}

TerrainSystem::TerrainSystem()
{
    Terrain::TerrainSystemServiceRequestBus::Handler::BusConnect();
    AZ::TickBus::Handler::BusConnect();

    m_currentSettings.m_systemActive = false;
    m_currentSettings.m_worldBounds = AZ::Aabb::CreateNull();

    m_requestedSettings = m_currentSettings;
    m_requestedSettings.m_worldBounds = AZ::Aabb::CreateFromMinMax(AZ::Vector3(-512.0f), AZ::Vector3(512.0f));
}

TerrainSystem::~TerrainSystem()
{
    AZ::TickBus::Handler::BusDisconnect();
    Terrain::TerrainSystemServiceRequestBus::Handler::BusDisconnect();

    Deactivate();
}

void TerrainSystem::Activate()
{
    AzFramework::Terrain::TerrainDataNotificationBus::Broadcast(
        &AzFramework::Terrain::TerrainDataNotificationBus::Events::OnTerrainDataCreateBegin);

    m_dirtyRegion = AZ::Aabb::CreateNull();
    m_terrainHeightDirty = true;
    m_terrainSettingsDirty = true;
    m_requestedSettings.m_systemActive = true;

    {
        AZStd::shared_lock<AZStd::shared_mutex> lock(m_areaMutex);
        m_registeredAreas.clear();
    }

    AzFramework::Terrain::TerrainDataRequestBus::Handler::BusConnect();

    // Register any terrain spawners that were already active before the terrain system activated.
    auto enumerationCallback = [&]([[maybe_unused]] Terrain::TerrainSpawnerRequests* terrainSpawner) -> bool
    {
        AZ::EntityId areaId = *(Terrain::TerrainSpawnerRequestBus::GetCurrentBusId());
        RegisterArea(areaId);

        // Keep Enumerating
        return true;
    };
    Terrain::TerrainSpawnerRequestBus::EnumerateHandlers(enumerationCallback);

    AzFramework::Terrain::TerrainDataNotificationBus::Broadcast(
        &AzFramework::Terrain::TerrainDataNotificationBus::Events::OnTerrainDataCreateEnd);
}

void TerrainSystem::Deactivate()
{
    AzFramework::Terrain::TerrainDataNotificationBus::Broadcast(
        &AzFramework::Terrain::TerrainDataNotificationBus::Events::OnTerrainDataDestroyBegin);

    AzFramework::Terrain::TerrainDataRequestBus::Handler::BusDisconnect();

    {
        AZStd::shared_lock<AZStd::shared_mutex> lock(m_areaMutex);
        m_registeredAreas.clear();
    }

    m_dirtyRegion = AZ::Aabb::CreateNull();
    m_terrainHeightDirty = true;
    m_terrainSettingsDirty = true;
    m_requestedSettings.m_systemActive = false;

    if (auto rpi = AZ::RPI::RPISystemInterface::Get(); rpi)
    {
        if (auto defaultScene = rpi->GetDefaultScene(); defaultScene)
        {
            const AZ::RPI::Scene* scene = defaultScene.get();
            if (auto terrainFeatureProcessor = scene->GetFeatureProcessor<TerrainFeatureProcessor>(); terrainFeatureProcessor)
            {
                terrainFeatureProcessor->RemoveTerrainData();
            }
        }
    }

    AzFramework::Terrain::TerrainDataNotificationBus::Broadcast(
        &AzFramework::Terrain::TerrainDataNotificationBus::Events::OnTerrainDataDestroyEnd);
}

void TerrainSystem::SetTerrainAabb(const AZ::Aabb& worldBounds)
{   
    m_requestedSettings.m_worldBounds = worldBounds;
    m_terrainSettingsDirty = true;
}

void TerrainSystem::SetTerrainHeightQueryResolution(AZ::Vector2 queryResolution)
{
    m_requestedSettings.m_heightQueryResolution = queryResolution;
    m_terrainSettingsDirty = true;
}

AZ::Aabb TerrainSystem::GetTerrainAabb() const
{
    return m_currentSettings.m_worldBounds;
}

AZ::Vector2 TerrainSystem::GetTerrainHeightQueryResolution() const
{
    return m_currentSettings.m_heightQueryResolution;
}

void TerrainSystem::ClampPosition(float x, float y, AZ::Vector2& outPosition, AZ::Vector2& normalizedDelta) const
{
    // Given an input position, clamp the values to our terrain grid, where it will always go to the terrain grid point
    // at a lower value, whether positive or negative.  Ex: 3.3 -> 3, -3.3 -> -4
    // Also, return the normalized delta as a value of [0-1) describing what fraction of a grid point the value moved.

    // Scale the position by the query resolution, so that integer values represent exact steps on the grid,
    // and fractional values are the amount in-between each grid point, in the range [0-1).
    AZ::Vector2 normalizedPosition = AZ::Vector2(x, y) / m_currentSettings.m_heightQueryResolution;
    normalizedDelta = AZ::Vector2(
        normalizedPosition.GetX() - floor(normalizedPosition.GetX()), normalizedPosition.GetY() - floor(normalizedPosition.GetY()));

    // Remove the fractional part, then scale back down into world space.
    outPosition = (normalizedPosition - normalizedDelta) * m_currentSettings.m_heightQueryResolution;
}

float TerrainSystem::GetHeightSynchronous(float x, float y, Sampler sampler, bool* terrainExistsPtr) const
{
    bool terrainExists = false;
    float height = m_currentSettings.m_worldBounds.GetMin().GetZ();

    AZStd::shared_lock<AZStd::shared_mutex> lock(m_areaMutex);

    switch (sampler)
    {
    // Get the value at the requested location, using the terrain grid to bilinear filter between sample grid points.
    case AzFramework::Terrain::TerrainDataRequests::Sampler::BILINEAR:
        {
            // pos0 contains one corner of our grid square, pos1 contains the opposite corner, and normalizedDelta is the fractional
            // amount the position exists between those corners.
            // Ex: (3.3, 4.4) would have a pos0 of (3, 4), a pos1 of (4, 5), and a delta of (0.3, 0.4).
            AZ::Vector2 normalizedDelta;
            AZ::Vector2 pos0;
            ClampPosition(x, y, pos0, normalizedDelta);
            const AZ::Vector2 pos1 = pos0 + m_currentSettings.m_heightQueryResolution;

            const float heightX0Y0 = GetTerrainAreaHeight(pos0.GetX(), pos0.GetY(), terrainExists);
            const float heightX1Y0 = GetTerrainAreaHeight(pos1.GetX(), pos0.GetY(), terrainExists);
            const float heightX0Y1 = GetTerrainAreaHeight(pos0.GetX(), pos1.GetY(), terrainExists);
            const float heightX1Y1 = GetTerrainAreaHeight(pos1.GetX(), pos1.GetY(), terrainExists);
            const float heightXY0 = AZ::Lerp(heightX0Y0, heightX1Y0, normalizedDelta.GetX());
            const float heightXY1 = AZ::Lerp(heightX0Y1, heightX1Y1, normalizedDelta.GetX());
            height = AZ::Lerp(heightXY0, heightXY1, normalizedDelta.GetY());
        }
        break;

    //! Clamp the input point to the terrain sample grid, then get the height at the given grid location.
    case AzFramework::Terrain::TerrainDataRequests::Sampler::CLAMP:
        {
            AZ::Vector2 normalizedDelta;
            AZ::Vector2 clampedPosition;
            ClampPosition(x, y, clampedPosition, normalizedDelta);

            height = GetTerrainAreaHeight(clampedPosition.GetX(), clampedPosition.GetY(), terrainExists);
        }
        break;

    //! Directly get the value at the location, regardless of terrain sample grid density.
    case AzFramework::Terrain::TerrainDataRequests::Sampler::EXACT:
        [[fallthrough]];
    default:
        height = GetTerrainAreaHeight(x, y, terrainExists);
        break;
    }

    if (terrainExistsPtr)
    {
        *terrainExistsPtr = terrainExists;
    }

    return AZ::GetClamp(
        height, m_currentSettings.m_worldBounds.GetMin().GetZ(), m_currentSettings.m_worldBounds.GetMax().GetZ());
}

float TerrainSystem::GetTerrainAreaHeight(float x, float y, bool& terrainExists) const
{
    AZ::Vector3 inPosition((float)x, (float)y, m_currentSettings.m_worldBounds.GetMin().GetZ());
    float height = m_currentSettings.m_worldBounds.GetMin().GetZ();

    AZStd::shared_lock<AZStd::shared_mutex> lock(m_areaMutex);

    for (auto& [areaId, areaBounds] : m_registeredAreas)
    {
        inPosition.SetZ(areaBounds.GetMin().GetZ());
        if (areaBounds.Contains(inPosition))
        {
            AZ::Vector3 outPosition;
            Terrain::TerrainAreaHeightRequestBus::Event(
                areaId, &Terrain::TerrainAreaHeightRequestBus::Events::GetHeight, inPosition, outPosition, terrainExists);
            height = outPosition.GetZ();
            break;
        }
    }

    return height;
}

float TerrainSystem::GetHeight(AZ::Vector3 position, Sampler sampler, bool* terrainExistsPtr) const
{
    return GetHeightSynchronous(position.GetX(), position.GetY(), sampler, terrainExistsPtr);
}

float TerrainSystem::GetHeightFromFloats(float x, float y, Sampler sampler, bool* terrainExistsPtr) const
{
    return GetHeightSynchronous(x, y, sampler, terrainExistsPtr);
}

bool TerrainSystem::GetIsHoleFromFloats(float x, float y, Sampler sampler) const
{
    bool terrainExists = false;
    GetHeightSynchronous(x, y, sampler, &terrainExists);
    return !terrainExists;
}

AZ::Vector3 TerrainSystem::GetNormalSynchronous(float x, float y, Sampler sampler, bool* terrainExistsPtr) const
{
    AZStd::shared_lock<AZStd::shared_mutex> lock(m_areaMutex);

    bool terrainExists = false;

    AZ::Vector3 outNormal = AZ::Vector3::CreateAxisZ();

    const AZ::Vector2 range = (m_currentSettings.m_heightQueryResolution / 2.0f);
    const AZ::Vector2 left (x - range.GetX(), y);
    const AZ::Vector2 right(x + range.GetX(), y);
    const AZ::Vector2 up   (x, y - range.GetY());
    const AZ::Vector2 down (x, y + range.GetY());

    AZ::Vector3 v1(up.GetX(), up.GetY(), GetHeightSynchronous(up.GetX(), up.GetY(), sampler, &terrainExists));
    AZ::Vector3 v2(left.GetX(), left.GetY(), GetHeightSynchronous(left.GetX(), left.GetY(), sampler, &terrainExists));
    AZ::Vector3 v3(right.GetX(), right.GetY(), GetHeightSynchronous(right.GetX(), right.GetY(), sampler, &terrainExists));
    AZ::Vector3 v4(down.GetX(), down.GetY(), GetHeightSynchronous(down.GetX(), down.GetY(), sampler, &terrainExists));

    outNormal = (v3 - v2).Cross(v4 - v1).GetNormalized();

    if (terrainExistsPtr)
    {
        *terrainExistsPtr = terrainExists;
    }

    return outNormal;
}

AZ::Vector3 TerrainSystem::GetNormal(AZ::Vector3 position, Sampler sampler, bool* terrainExistsPtr) const
{
    return GetNormalSynchronous(position.GetX(), position.GetY(), sampler, terrainExistsPtr);
}

AZ::Vector3 TerrainSystem::GetNormalFromFloats(float x, float y, Sampler sampler, bool* terrainExistsPtr) const
{
    return GetNormalSynchronous(x, y, sampler, terrainExistsPtr);
}


AzFramework::SurfaceData::SurfaceTagWeight TerrainSystem::GetMaxSurfaceWeight(
    [[maybe_unused]] AZ::Vector3 position, [[maybe_unused]] Sampler sampleFilter, [[maybe_unused]] bool* terrainExistsPtr) const
{
    if (terrainExistsPtr)
    {
        *terrainExistsPtr = true;
    }

    return AzFramework::SurfaceData::SurfaceTagWeight();
}

AzFramework::SurfaceData::SurfaceTagWeight TerrainSystem::GetMaxSurfaceWeightFromFloats(
    [[maybe_unused]] float x,
    [[maybe_unused]] float y,
    [[maybe_unused]] Sampler sampleFilter,
    [[maybe_unused]] bool* terrainExistsPtr) const
{
    if (terrainExistsPtr)
    {
        *terrainExistsPtr = true;
    }

    return AzFramework::SurfaceData::SurfaceTagWeight();
}

const char* TerrainSystem::GetMaxSurfaceName(
    [[maybe_unused]] AZ::Vector3 position, [[maybe_unused]] Sampler sampleFilter, [[maybe_unused]] bool* terrainExistsPtr) const
{
    if (terrainExistsPtr)
    {
        *terrainExistsPtr = true;
    }

    return "";
}

/*
void TerrainSystem::GetSurfaceWeights(
    [[maybe_unused]] const AZ::Vector3& inPosition,
    [[maybe_unused]] Sampler sampleFilter,
    [[maybe_unused]] SurfaceData::SurfaceTagWeightMap& outSurfaceWeights)
{
    // TODO: implement
}

void TerrainSystem::GetSurfacePoint(
    const AZ::Vector3& inPosition, [[maybe_unused]] Sampler sampleFilter, SurfaceData::SurfacePoint& outSurfacePoint)
{
    // TODO: Handle sampleFilter

    float sampleX = inPosition.GetX();
    float sampleY = inPosition.GetY();

    GetHeight(inPosition, sampleFilter, outSurfacePoint.m_position);
    //outSurfacePoint.m_position = AZ::Vector3(sampleX, sampleY, GetHeightSynchronous(sampleX, sampleY));
    outSurfacePoint.m_normal = GetNormalSynchronous(sampleX, sampleY);
}




void TerrainSystem::ProcessHeightsFromRegion(const AZ::Aabb& inRegion, const AZ::Vector2 stepSize, Sampler sampleFilter, SurfacePointRegionFillCallback perPositionCallback, TerrainDataReadyCallback onComplete)
{
    // Don't bother processing if we don't have a callback
    if (!perPositionCallback)
    {
        return;
    }

    uint32_t numSamplesX = static_cast<uint32_t>((inRegion.GetMax().GetX() - inRegion.GetMin().GetX()) / stepSize.GetX());
    uint32_t numSamplesY = static_cast<uint32_t>((inRegion.GetMax().GetY() - inRegion.GetMin().GetY()) / stepSize.GetY());

    for (uint32_t y = 0; y < numSamplesY; y++)
    {
        for (uint32_t x = 0; x < numSamplesX; x++)
        {
            float fx = (float)(inRegion.GetMin().GetX() + (x * stepSize.GetX()));
            float fy = (float)(inRegion.GetMin().GetY() + (y * stepSize.GetY()));

            SurfaceData::SurfacePoint surfacePoint;
            GetHeight(AZ::Vector3(fx, fy, 0.0f), sampleFilter, surfacePoint.m_position);
            perPositionCallback(surfacePoint, x, y);
        }
    }

    if (onComplete)
    {
        onComplete();
    }
}


void TerrainSystem::ProcessSurfacePointsFromRegion(const AZ::Aabb& inRegion, const AZ::Vector2 stepSize, Sampler sampleFilter, SurfacePointRegionFillCallback perPositionCallback, TerrainDataReadyCallback onComplete)
{
    // Don't bother processing if we don't have a callback
    if (!perPositionCallback)
    {
        return;
    }

    uint32_t numSamplesX = static_cast<uint32_t>((inRegion.GetMax().GetX() - inRegion.GetMin().GetX()) / stepSize.GetX());
    uint32_t numSamplesY = static_cast<uint32_t>((inRegion.GetMax().GetY() - inRegion.GetMin().GetY()) / stepSize.GetY());

    for (uint32_t y = 0; y < numSamplesY; y++)
    {
        for (uint32_t x = 0; x < numSamplesX; x++)
        {
            float fx = (float)(inRegion.GetMin().GetX() + (x * stepSize.GetX()));
            float fy = (float)(inRegion.GetMin().GetY() + (y * stepSize.GetY()));

            SurfaceData::SurfacePoint surfacePoint;
            GetSurfacePoint(AZ::Vector3(fx, fy, inRegion.GetMin().GetZ()), sampleFilter, surfacePoint);
            perPositionCallback(surfacePoint, x, y);
        }
    }

    if (onComplete)
    {
        onComplete();
    }
}
*/

void TerrainSystem::RegisterArea(AZ::EntityId areaId)
{
    AZStd::unique_lock<AZStd::shared_mutex> lock(m_areaMutex);
    AZ::Aabb aabb = AZ::Aabb::CreateNull();
    LmbrCentral::ShapeComponentRequestsBus::EventResult(aabb, areaId, &LmbrCentral::ShapeComponentRequestsBus::Events::GetEncompassingAabb);
    m_registeredAreas[areaId] = aabb;
    m_dirtyRegion.AddAabb(aabb);
    m_terrainHeightDirty = true;
}

void TerrainSystem::UnregisterArea(AZ::EntityId areaId)
{
    AZStd::unique_lock<AZStd::shared_mutex> lock(m_areaMutex);

    // Remove the data for this entity from the registered areas.
    // Erase_if is used as erase would use the comparator to lookup the entity id in the map.
    // As the comparator will get the new layer/priority data for the entity, the id lookup will fail.
    AZStd::erase_if(
        m_registeredAreas,
        [areaId, this](const auto& item)
        {
            auto const& [entityId, aabb] = item;
            if (areaId == entityId)
            {
                m_dirtyRegion.AddAabb(aabb);
                m_terrainHeightDirty = true;
                return true;
            }
            return false;
        });
}

void TerrainSystem::RefreshArea(AZ::EntityId areaId)
{
    AZStd::unique_lock<AZStd::shared_mutex> lock(m_areaMutex);

    auto areaAabb = m_registeredAreas.find(areaId);

    AZ::Aabb oldAabb = (areaAabb != m_registeredAreas.end()) ? areaAabb->second : AZ::Aabb::CreateNull();
    AZ::Aabb newAabb = AZ::Aabb::CreateNull();
    LmbrCentral::ShapeComponentRequestsBus::EventResult(newAabb, areaId, &LmbrCentral::ShapeComponentRequestsBus::Events::GetEncompassingAabb);
    m_registeredAreas[areaId] = newAabb;

    AZ::Aabb expandedAabb = oldAabb;
    expandedAabb.AddAabb(newAabb);

    m_dirtyRegion.AddAabb(expandedAabb);
    m_terrainHeightDirty = true;
}

void TerrainSystem::OnTick(float /*deltaTime*/, AZ::ScriptTimePoint /*time*/)
{
    bool terrainSettingsChanged = false;

    if (m_terrainSettingsDirty)
    {
        terrainSettingsChanged = true;
        m_terrainSettingsDirty = false;

        // This needs to happen before the "system active" check below, because activating the system will cause the various
        // terrain layer areas to request the current world bounds.
        if (m_requestedSettings.m_worldBounds != m_currentSettings.m_worldBounds)
        {
            m_dirtyRegion = m_currentSettings.m_worldBounds;
            m_dirtyRegion.AddAabb(m_requestedSettings.m_worldBounds);
            m_terrainHeightDirty = true;
            m_currentSettings.m_worldBounds = m_requestedSettings.m_worldBounds;
        }

        if (m_requestedSettings.m_heightQueryResolution != m_currentSettings.m_heightQueryResolution)
        {
            m_dirtyRegion = AZ::Aabb::CreateNull();
            m_terrainHeightDirty = true;
        }

        m_currentSettings = m_requestedSettings;
    }

    if (m_currentSettings.m_systemActive && m_terrainHeightDirty)
    {
        AZStd::shared_lock<AZStd::shared_mutex> lock(m_areaMutex);

        // Block other threads from accessing the surface data bus while we are in GetValue (which may call into the SurfaceData bus).
        // We lock our surface data mutex *before* checking / setting "isRequestInProgress" so that we prevent race conditions
        // that create false detection of cyclic dependencies when multiple requests occur on different threads simultaneously.
        // (One case where this was previously able to occur was in rapid updating of the Preview widget on the
        // GradientSurfaceDataComponent in the Editor when moving the threshold sliders back and forth rapidly)
        auto& surfaceDataContext = SurfaceData::SurfaceDataSystemRequestBus::GetOrCreateContext(false);
        typename SurfaceData::SurfaceDataSystemRequestBus::Context::DispatchLockGuard scopeLock(surfaceDataContext.m_contextMutex);

        AZ::Transform transform = AZ::Transform::CreateTranslation(m_currentSettings.m_worldBounds.GetCenter());

        uint32_t width = aznumeric_cast<uint32_t>(
            (float)m_currentSettings.m_worldBounds.GetXExtent() / m_currentSettings.m_heightQueryResolution.GetX());
        uint32_t height = aznumeric_cast<uint32_t>(
            (float)m_currentSettings.m_worldBounds.GetYExtent() / m_currentSettings.m_heightQueryResolution.GetY());
        AZStd::vector<float> pixels;
        pixels.resize_no_construct(width * height);
        const uint32_t pixelDataSize = width * height * sizeof(float);
        memset(pixels.data(), 0, pixelDataSize);

        for (uint32_t y = 0; y < height; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                bool terrainExists;
                float terrainHeight = GetTerrainAreaHeight(
                    (x * m_currentSettings.m_heightQueryResolution.GetX()) + m_currentSettings.m_worldBounds.GetMin().GetX(),
                    (y * m_currentSettings.m_heightQueryResolution.GetY()) + m_currentSettings.m_worldBounds.GetMin().GetY(),
                    terrainExists);

                pixels[(y * width) + x] =
                    (terrainHeight - m_currentSettings.m_worldBounds.GetMin().GetZ()) /
                    m_currentSettings.m_worldBounds.GetExtents().GetZ();
            }
        }
        
        if (auto rpi = AZ::RPI::RPISystemInterface::Get(); rpi)
        {
            if (auto defaultScene = rpi->GetDefaultScene(); defaultScene)
            {
                const AZ::RPI::Scene* scene = defaultScene.get();
                if (auto terrainFeatureProcessor = scene->GetFeatureProcessor<TerrainFeatureProcessor>(); terrainFeatureProcessor)
                {
                    terrainFeatureProcessor->UpdateTerrainData(
                        transform, m_currentSettings.m_worldBounds, m_currentSettings.m_heightQueryResolution.GetX(), width, height,
                        pixels);
                }
            }
        }
    }

    if (terrainSettingsChanged || m_terrainHeightDirty)
    {
        // Block other threads from accessing the surface data bus while we are in GetValue (which may call into the SurfaceData bus).
        // We lock our surface data mutex *before* checking / setting "isRequestInProgress" so that we prevent race conditions
        // that create false detection of cyclic dependencies when multiple requests occur on different threads simultaneously.
        // (One case where this was previously able to occur was in rapid updating of the Preview widget on the
        // GradientSurfaceDataComponent in the Editor when moving the threshold sliders back and forth rapidly)
        auto& surfaceDataContext = SurfaceData::SurfaceDataSystemRequestBus::GetOrCreateContext(false);
        typename SurfaceData::SurfaceDataSystemRequestBus::Context::DispatchLockGuard scopeLock(surfaceDataContext.m_contextMutex);

        AzFramework::Terrain::TerrainDataNotifications::TerrainDataChangedMask changeMask =
            AzFramework::Terrain::TerrainDataNotifications::TerrainDataChangedMask::None;

        if (terrainSettingsChanged)
        {
            changeMask = static_cast<AzFramework::Terrain::TerrainDataNotifications::TerrainDataChangedMask>(
                changeMask | AzFramework::Terrain::TerrainDataNotifications::TerrainDataChangedMask::Settings);
        }
        if (m_terrainHeightDirty)
        {
            changeMask = static_cast<AzFramework::Terrain::TerrainDataNotifications::TerrainDataChangedMask>(
                changeMask | AzFramework::Terrain::TerrainDataNotifications::TerrainDataChangedMask::HeightData);
        }

        // Make sure to set these *before* calling OnTerrainDataChanged, since it's possible that subsystems reacting to that call will
        // cause the data to become dirty again.
        AZ::Aabb dirtyRegion = m_dirtyRegion;
        m_terrainHeightDirty = false;
        m_dirtyRegion = AZ::Aabb::CreateNull();

        AzFramework::Terrain::TerrainDataNotificationBus::Broadcast(
            &AzFramework::Terrain::TerrainDataNotificationBus::Events::OnTerrainDataChanged, dirtyRegion,
            changeMask);
    }

}
