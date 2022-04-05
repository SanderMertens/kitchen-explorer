#include <kitchen_explorer.h>
#include <iostream>
#include <math.h>

const int TableXCount = 6;
const int TableYCount = 4;
const float TableSpacing = 5;
const int ChefCount = 10;
const int WaiterCount = 4;
const float GuestFrequency = 5; // Hz
const int GuestPartySize = 5;
const float PlatePreparationTime = 8.0; // sec
const float WaiterSpeed = 1.0;
const float DiningTime = 60.0;
const float PlateInitialTemperature = 80;
const float PlateCooldownFactor = 0.01; // deg/sec
const float PlateTemperatureThreshold = 55;
const float ColdPlateHappinessPenalty = 0.25;
const float RoomTemperature = 20;
const float HappinessCooldown = 0.01;

namespace kitchen_explorer {

struct Plate { };
struct Table { };
struct Chef { };
struct Waiter { };
struct Guest { };

enum class PlateStatus {
    Preparing,
    Ready,
    InUse
};

enum class TableStatus {
    Unoccupied,
    Unassigned,
    Waiting,
    Dining
};

enum class ChefStatus {
    Idle,
    Cooking
};

enum class WaiterStatus {
    Idle,
    WalkingToTable,
    WalkingToKitchen
};

struct ProgressTracker {
    float cur;
    float expire;
};

struct DistanceFromKitchen {
    float value;
};

struct Temperature {
    float value;
};

struct Position {
    float x;
    float y;
};

struct Happiness {
    float value;
};

int app(int argc, char *argv[]) {
    flecs::world ecs(argc, argv);

    ecs.import<flecs::units>();

    flecs::log::set_level(0);
    
    auto m = ecs.entity("::kitchen_explorer").add(flecs::Module);

    // Lookup (query) identifiers in kitchen_explorer namespace 
    flecs::entity_t lookup_path[3] = { EcsFlecsCore, m, 0 };
    ecs.set_lookup_path(lookup_path);

    // Register components
    ecs.component<Position>()
        .member<float>("x")
        .member<float>("y");

    ecs.component<ProgressTracker>()
        .member<float, flecs::units::duration::Seconds>("cur")
        .member<float, flecs::units::duration::Seconds>("expire");

    ecs.component<DistanceFromKitchen>()
        .member<float, flecs::units::length::Meters>("value");

    ecs.component<Temperature>()
        .member<float, flecs::units::temperature::Celsius>("value");

    ecs.component<Happiness>()
        .member<float, flecs::units::Percentage>("value");

    // Root scopes
    auto tables = ecs.entity("::tables");
    auto chefs = ecs.entity("::chefs");
    auto waiters = ecs.entity("::waiters");
    auto plates = ecs.entity("::plates");

    // Create tables
    float TableXH = TableXCount / 2.0;
    float TableYH = TableYCount / 2.0;
    for (int x = -TableXH; x < TableXH; x ++) {
        for (int y = -TableYH; y < TableYH; y ++) {
            ecs.entity().child_of(tables)
                .add<Table>()
                .add(TableStatus::Unoccupied)
                .set<Position>({x * TableSpacing, y * TableSpacing});
        }
    }
    
    // Create chefs
    for (int i = 0; i < ChefCount; i ++) {
        ecs.entity().child_of(chefs)
            .add<Chef>()
            .add(ChefStatus::Idle);
    }

    // Create waiters
    for (int i = 0; i < WaiterCount; i ++) {
        ecs.entity().child_of(waiters)
            .add<Waiter>()
            .add(WaiterStatus::Idle)
            .set<DistanceFromKitchen>({0});
    }

    // Increase progress tracker (used as timer to insert delays)
    ecs.system<ProgressTracker>("systems::IncreaseProgressTracker")
        .each([](flecs::iter& it, size_t, ProgressTracker& pt) {
            pt.cur += it.delta_time();
        });

    // Guest generator
    ecs.system("systems::GuestGenerator")
        .interval(GuestFrequency)
        .iter([](flecs::iter& it) {
            flecs::entity table;

            // Find free table
            it.world().filter_builder()
                .term<Table>()
                .term<TableStatus>(TableStatus::Unoccupied)
                .build()
                .each([&](flecs::entity t) {
                    table = t;
                });

            if (table) {
                table.add(TableStatus::Unassigned);

                int party_size = 1 + (rand() % GuestPartySize);
                for (int i = 0; i < party_size; i ++) {
                    it.world().entity().child_of(table)
                        .add<Guest>();
                    table.set<Happiness>({1});
                }
            }
        });

    // Assign idle chefs to waiting tables
    ecs.system("systems::AssignChef")
        .term<Table>()
        .term<TableStatus>(TableStatus::Unassigned)
        .no_staging()
        .iter([](flecs::iter& it) {
            it.world().defer_end();

            auto idle_chefs = it.world().filter_builder()
                .term<Chef>()
                .term<ChefStatus>(ChefStatus::Idle)
                .build();

            for (int i : it) {
                flecs::entity table = it.entity(i);

                // Find idle chef
                flecs::entity chef;
                idle_chefs.each([&](flecs::entity e) {
                    chef = e;
                });

                // Assign chef to table
                if (chef) {
                    chef.add<Table>(table);
                    chef.add(ChefStatus::Cooking);
                    table.add(TableStatus::Waiting);
                }
            }

            it.world().defer_begin();
        });

    // Create plate
    ecs.system("systems::CreatePlate")
        .term<Chef>()
        .term<ChefStatus>(ChefStatus::Cooking)
        .term<Plate>(flecs::Wildcard).oper(flecs::Not)
        .each([&](flecs::iter& it, size_t index) {
            auto ecs = it.world();
            flecs::entity chef = it.entity(index);
            
            // Lookup party size from table
            auto table = chef.get_object<Table>();
            int party_size = ecs.count(ecs.pair(flecs::ChildOf, table));
            
            // Create plate for table
            auto plate = it.world().entity()
                .child_of(plates)
                .add<Plate>()
                .add(PlateStatus::Preparing);

            // Assign plate to chef
            chef.add<Plate>(plate);

            // Initialize progress tracker
            chef.set<ProgressTracker>({0, party_size * PlatePreparationTime});
        });

    // Prepare plate
    ecs.system<ProgressTracker>("systems::PreparePlate")
        .term<Chef>()
        .term<Plate>(flecs::Wildcard)
        .each([](flecs::iter& it, size_t index, ProgressTracker& pt) {
            flecs::entity chef = it.entity(index);

            if (pt.cur > pt.expire) {
                auto table = chef.get_object<Table>();
                auto plate = chef.get_object<Plate>();

                // Add table to plate, marking it ready
                plate.add<Table>(table);
                plate.add(PlateStatus::Ready);
                plate.set<Temperature>({PlateInitialTemperature});

                // Chef is ready for the next plate
                chef.add(ChefStatus::Idle);
                chef.remove<Table>(table);
                chef.remove<Plate>(plate);
                chef.remove<ProgressTracker>();
            }
        });

    // Find idle waiter to pickup plate
    ecs.system("systems::AssignWaiter")
        .term<Plate>()
        .term<Table>(flecs::Wildcard)
        .term<Waiter>(flecs::Wildcard).oper(flecs::Not)
        .term<PlateStatus>(PlateStatus::Ready)
        .no_staging()
        .iter([](flecs::iter& it) {
            it.world().defer_end();

            auto idle_waiters = it.world().filter_builder()
                .term<Waiter>()
                .term<WaiterStatus>(WaiterStatus::Idle)
                .build();

            for (int i : it) {
                flecs::entity plate = it.entity(i);

                // Find idle waiter
                flecs::entity waiter;
                idle_waiters.each([&](flecs::entity e) {
                    waiter = e;
                });

                // Assign waiter to table
                if (waiter) {
                    flecs::entity table = plate.get_object<Table>();
                    waiter.add<Table>(table);
                    plate.add<Waiter>(waiter);

                    // First pick up plate
                    waiter.add(WaiterStatus::WalkingToKitchen);
                }
            }

            it.world().defer_begin();
        });

    // Happiness cooldown
    ecs.system<Happiness>("systems::HappinessCooldown")
        .term<Table>()
        .term<TableStatus>(TableStatus::Dining).oper(flecs::Not)
        .each([](flecs::iter& it, size_t, Happiness& h) {
            h.value -= HappinessCooldown * it.delta_time();
            if (h.value < 0) {
                h.value = 0; // not good
            }
        });

    // Plate cooldown
    ecs.system<Temperature>("systems::TemperatureCooldown")
        .term<Plate>()
        .each([](flecs::iter& it, size_t, Temperature& t) {
            t.value -= (t.value - RoomTemperature) 
                * PlateCooldownFactor
                * it.delta_time();
        });

    // Waiter walking to kitchen
    ecs.system<DistanceFromKitchen>("systems::WaiterToKitchen")
        .term<Waiter>()
        .term<WaiterStatus>(WaiterStatus::WalkingToKitchen)
        .each([](flecs::iter& it, size_t index, DistanceFromKitchen& d) {
            d.value -= WaiterSpeed * it.delta_time();
            if (d.value <= 0) {
                d.value = 0;

                flecs::entity waiter = it.entity(index);
                flecs::entity table = waiter.get_object<Table>();

                // Find plate for table (should be only one)
                flecs::entity plate;
                it.world().filter_builder()
                    .term<Plate>()
                    .term<Table>(table)
                    .build()
                    .each([&](flecs::entity e) {
                        plate = e;
                    });
                
                if (plate) {
                    waiter.add(WaiterStatus::WalkingToTable);
                    waiter.add<Plate>(plate);

                    const Position *table_pos = table.get<Position>();
                    float table_distance = sqrt(table_pos->x * table_pos->x +
                        table_pos->y * table_pos->y);

                    waiter.set<ProgressTracker>({
                        0, table_distance / WaiterSpeed});
                }
            }
        });

    // Waiter walking to table
    ecs.system<ProgressTracker, DistanceFromKitchen>("systems::WaiterToTable")
        .term<Waiter>()
        .term<WaiterStatus>(WaiterStatus::WalkingToTable)
        .each([](flecs::iter& it, size_t index, ProgressTracker &pt, DistanceFromKitchen& d) {
            d.value += it.delta_time() * WaiterSpeed;
            if (pt.cur >= pt.expire) {
                flecs::entity waiter = it.entity(index);
                flecs::entity table = waiter.get_object<Table>();
                flecs::entity plate = waiter.get_object<Plate>();

                table.add<Plate>(plate);
                waiter.remove<Table>(table);
                waiter.remove<Plate>(plate);
                plate.remove<Waiter>(waiter);
                waiter.add(WaiterStatus::Idle);
                plate.add(PlateStatus::InUse);
                table.add(TableStatus::Dining);
                table.set<ProgressTracker>({0, DiningTime});

                // If plate is cold subtract happiness
                const Temperature *t = plate.get<Temperature>();
                if (t->value < PlateTemperatureThreshold) {
                    Happiness *h = table.get_mut<Happiness>();
                    h->value -= ColdPlateHappinessPenalty;
                    if (h->value < 0) {
                        h->value = 0; // not good
                    }
                }
            }
        });

    // Guests are leaving
    ecs.system<ProgressTracker>("systems::GuestsLeaving")
        .term<Table>()
        .term<TableStatus>(TableStatus::Dining)
        .each([](flecs::iter&it, size_t index, ProgressTracker& pt) {
            if (pt.cur >= pt.expire) {
                flecs::entity table = it.entity(index);
                it.world().delete_with(it.world().pair(flecs::ChildOf, table));
                table.remove<Happiness>();
            }
        });

    // Table is dining
    ecs.system<ProgressTracker>("systems::Dine")
        .term<Table>()
        .term<TableStatus>(TableStatus::Dining)
        .each([](flecs::iter&it, size_t index, ProgressTracker& pt) {
            if (pt.cur >= pt.expire) {
                flecs::entity table = it.entity(index);
                flecs::entity plate = table.get_object<Plate>();
                table.add(TableStatus::Unoccupied);
                table.remove<ProgressTracker>();
                plate.destruct();
            }
        });

    // Run the app
    return ecs.app()
        .target_fps(60)
        .enable_rest()
        .run();
}
}

int main(int argc, char *argv[]) {
    return kitchen_explorer::app(argc, argv);
}
