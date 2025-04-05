#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <limits.h>
#define MAX_CRANES 30
#define MAX_DOCKS 30
#define MAX_CARGO_COUNT 200
#define MAX_NEW_REQUESTS 50
#define MAX_AUTH_STRING_LEN 100

typedef struct
{
    int id;
    int category;
    int craneCapacities[MAX_CRANES];
    int assignedShip;               //(-1 if empty)
    int remainingCargo;             // Remaining cargo to unload
    struct ShipRequest *dockedShip; // Pointer to the docked ship request
    int dockedTimestep;             // Timestep when the ship docked
} Dock;

typedef struct MessageStruct
{
    long mtype;
    int timestep;
    int shipId;
    int direction;
    int dockId;
    int cargoId;
    int isFinished;
    union
    {
        int numShipRequests;
        int craneId;
    };
} MessageStruct;

typedef struct ShipRequest
{
    int shipId;
    int timestep;
    int category;
    int direction;
    int emergency;
    int waitingTime;
    int numCargo;
    int cargo[MAX_CARGO_COUNT];
} ShipRequest;

typedef struct MainSharedMemory
{
    char authStrings[MAX_DOCKS][MAX_AUTH_STRING_LEN];
    ShipRequest newShipRequests[MAX_NEW_REQUESTS];
} MainSharedMemory;

typedef struct SolverRequest
{
    long mtype;
    int dockId;
    char authStringGuess[MAX_AUTH_STRING_LEN];
} SolverRequest;

typedef struct SolverResponse
{
    long mtype;
    int guessIsCorrect;
} SolverResponse;

int compareShipRequests(const void *a, const void *b)
{
    ShipRequest *requestA = (ShipRequest *)a;
    ShipRequest *requestB = (ShipRequest *)b;
    return requestA->waitingTime - requestB->waitingTime;
}

typedef struct
{
    ShipRequest **data; // Array to store pointers to ShipRequest objects
    int capacity;       // Maximum possible size of the heap
    int size;           // Current number of elements in the heap
} PriorityQueue;

static inline int parent(int i) { return (i - 1) / 2; }
static inline int leftChild(int i) { return 2 * i + 1; }
static inline int rightChild(int i) { return 2 * i + 2; }

PriorityQueue *createPQ(int capacity)
{
    PriorityQueue *pq = malloc(sizeof(PriorityQueue));
    if (!pq)
    {
        perror("Failed to allocate PriorityQueue");
        exit(EXIT_FAILURE);
    }
    pq->data = malloc(capacity * sizeof(ShipRequest *));
    if (!pq->data)
    {
        perror("Failed to allocate data array");
        free(pq);
        exit(EXIT_FAILURE);
    }
    pq->capacity = capacity;
    pq->size = 0;
    return pq;
}

void destroyPQ(PriorityQueue *pq)
{
    if (pq)
    {
        free(pq->data);
        free(pq);
    }
}

static void heapifyUp(PriorityQueue *pq, int index)
{
    while (index != 0 && pq->data[parent(index)]->waitingTime > pq->data[index]->waitingTime)
    {
        ShipRequest *temp = pq->data[index];
        pq->data[index] = pq->data[parent(index)];
        pq->data[parent(index)] = temp;
        index = parent(index);
    }
}

static void heapifyDown(PriorityQueue *pq, int index)
{
    int smallest = index;
    int left = leftChild(index);
    int right = rightChild(index);

    if (left < pq->size && pq->data[left]->waitingTime < pq->data[smallest]->waitingTime)
        smallest = left;
    if (right < pq->size && pq->data[right]->waitingTime < pq->data[smallest]->waitingTime)
        smallest = right;
    if (smallest != index)
    {
        ShipRequest *temp = pq->data[index];
        pq->data[index] = pq->data[smallest];
        pq->data[smallest] = temp;
        heapifyDown(pq, smallest);
    }
}

void insertPQ(PriorityQueue *pq, ShipRequest *request)
{
    if (pq->size == pq->capacity)
    {
        fprintf(stderr, "Priority Queue is full. Cannot insert Ship ID %d\n", request->shipId);
        return;
    }
    pq->data[pq->size] = request;
    pq->size++;
    heapifyUp(pq, pq->size - 1);
}

ShipRequest *extractMinPQ(PriorityQueue *pq)
{
    if (pq->size <= 0)
    {
        fprintf(stderr, "Priority Queue is empty.\n");
        return NULL;
    }
    ShipRequest *minRequest = pq->data[0];
    pq->data[0] = pq->data[pq->size - 1];
    pq->size--;
    heapifyDown(pq, 0);
    return minRequest;
}

int isEmptyPQ(PriorityQueue *pq)
{
    return pq->size == 0;
}

PriorityQueue *pq;

Dock *findBestDock(ShipRequest *ship, int numDocks, Dock *docks)
{
    Dock *bestDock = NULL;
    int minCategory = INT_MAX;

    for (int i = 0; i < numDocks; i++)
    {
        if (docks[i].assignedShip == -1 && docks[i].category >= ship->category)
        {
            if (docks[i].category < minCategory)
            {
                minCategory = docks[i].category;
                bestDock = &docks[i];
            }
        }
    }

    return bestDock;
}

void simulateCargoMovement(int numDocks, Dock *docks, int validationMsgQID, int currentTimestep)
{
    printf("\n-- Simulating cargo movement for current timestep --\n");

    for (int i = 0; i < numDocks; i++)
    {
        printf("Dock %d: Ship ID Parked: %d \n", i, docks[i].assignedShip);
        if (docks[i].assignedShip != -1 && docks[i].dockedShip != NULL && docks[i].dockedTimestep < currentTimestep)
        {
            ShipRequest *ship = docks[i].dockedShip;
            printf("Docked Ship ID %d at Dock %d with category %d\n", ship->shipId, docks[i].id, ship->category);
            // Track which cranes have been used in this timestep
            int cranesUsed[docks[i].category];
            for (int c = 0; c < docks[i].category; c++)
            {
                cranesUsed[c] = 0;
            }

            // For each crane, try to move one eligible cargo
            for (int c = 0; c < docks[i].category; c++)
            {
                if (cranesUsed[c])
                    continue;

                int craneCap = docks[i].craneCapacities[c];

                // Find a cargo that is not yet serviced and this crane can move
                for (int k = 0; k < ship->numCargo; k++)
                {
                    if (ship->cargo[k] != -1 && ship->cargo[k] <= craneCap)
                    {
                        printf("Dock %d, Crane %d (cap %d) moves cargo item %d (weight %d) from Ship ID %d\n",
                               i, c, craneCap, k, ship->cargo[k], ship->shipId);

                        MessageStruct msg;
                        msg.mtype = 4;
                        msg.dockId = docks[i].id;
                        msg.shipId = ship->shipId;
                        msg.cargoId = k;
                        msg.direction = ship->direction;
                        msg.craneId = c;
                        usleep(3000);

                        if (!msgsnd(validationMsgQID, &msg, sizeof(msg) - sizeof(long), 0))
                        {
                            printf("Sent cargo movement message for Ship ID %d, Cargo ID %d to Dock %d\n", ship->shipId, k, i);
                        }
                        else
                        {
                            perror("Failed to send cargo movement message");
                            exit(EXIT_FAILURE);
                        }

                        ship->cargo[k] = -1; // Mark as moved
                        docks[i].remainingCargo--;
                        cranesUsed[c] = 1;
                        break; // Move to next crane
                    }
                }
            }

            // If all cargo is moved, undock the ship
            if (docks[i].remainingCargo <= 0)
            {
                printf("Ship ID %d finished unloading at Dock %d and undocks.\n", ship->shipId, i);
                docks[i].assignedShip = -1;
                docks[i].dockedShip = NULL;
                docks[i].remainingCargo = 0;
            }
        }
    }
}

void assignWaitingShips(int numDocks, Dock *docks, int validationMsgQID)
{
    while (!isEmptyPQ(pq))
    {
        ShipRequest *ship = extractMinPQ(pq);
        Dock *freeDock = findBestDock(ship, numDocks, docks);
        if (freeDock)
        {
            ShipRequest *shipCopy = malloc(sizeof(ShipRequest));
            *shipCopy = *ship;

            freeDock->assignedShip = shipCopy->shipId;

            freeDock->dockedShip = shipCopy;
            freeDock->remainingCargo = shipCopy->numCargo; // Initialize remaining cargo
            freeDock->dockedTimestep = shipCopy->timestep;
            printf("Assigned waiting Ship ID %d to Dock ID %d with category %d\n", shipCopy->shipId, freeDock->id, freeDock->category);

            MessageStruct msg;
            msg.mtype = 2;
            msg.dockId = freeDock->id;
            msg.shipId = shipCopy->shipId;
            msg.direction = shipCopy->direction;

            usleep(3000);
            if (!msgsnd(validationMsgQID, &msg, sizeof(MessageStruct) - sizeof(long), 0))
            {
                printf("Sent assignment message for Ship ID %d to Dock %d\n", shipCopy->shipId, freeDock->id);
            }
            else
            {
                perror("Docking failed to send message");
                exit(EXIT_FAILURE);
            }
        }
        else
        {
            ShipRequest *shipCopy = malloc(sizeof(ShipRequest));
            *shipCopy = *ship;
            // No suitable dock found, reinsert the ship and break
            insertPQ(pq, shipCopy);
            break;
        }
    }
}

void processNewShipRequests(int sharedMemKey, MessageStruct msg, int numDocks, Dock *docks, int validationMsgQID, int currentTimestep)
{
    printf("\n== New timestep message received (Timestep: %d) ==\n", msg.timestep);

    // Attach shared memory and process new ship requests.
    int id = shmget(sharedMemKey, sizeof(MainSharedMemory), IPC_CREAT | 0666);
    if (id == -1)
    {
        perror("shmget");
        return;
    }
    MainSharedMemory *mainMem = (MainSharedMemory *)shmat(id, NULL, 0);
    if (mainMem == (void *)-1)
    {
        perror("shmat");
        return;
    }
    printf("Attached shared memory for new requests\n");

    // Insert each new request into the waiting queue or assign immediately.
    for (int i = 0; i < msg.numShipRequests; i++)
    {
        ShipRequest *request = &mainMem->newShipRequests[i];
        printf("Received New Ship Request: ID %d, Category %d\n", request->shipId, request->category);
        insertPQ(pq, request);
    }

    assignWaitingShips(numDocks, docks, validationMsgQID);
    simulateCargoMovement(numDocks, docks, validationMsgQID, currentTimestep);

    // Tell validation that go to next timestep

    MessageStruct msgToValidation;
    msgToValidation.mtype = 5;
    usleep(3000);

    if (!msgsnd(validationMsgQID, &msgToValidation, sizeof(msgToValidation) - sizeof(long), 0))
    {
        printf("Sent message to validation to go to next timestep\n");
    }
    else
    {
        perror("Failed to send message to validation");
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[])
{
    // if (argc != 2)
    // {
    //     fprintf(stderr, "Need testcase number as argument\n");
    //     return EXIT_FAILURE;
    // }

    int testcase = 3; // atoi(argv[1]);

    char filename[20];
    snprintf(filename, sizeof(filename), "testcase%d/input.txt", testcase);
    FILE *file = fopen(filename, "r");
    pq = createPQ(1000000);
    if (!file)
    {
        perror("Failed to open file");
        return EXIT_FAILURE;
    }

    int sharedMemKey, mainMsgQKey;
    int totalSolvers, numDocks;

    fscanf(file, "%d", &sharedMemKey);
    fscanf(file, "%d", &mainMsgQKey);

    fscanf(file, "%d", &totalSolvers);
    int solverMsgQKeys[totalSolvers];
    for (int i = 0; i < totalSolvers; i++)
    {
        fscanf(file, "%d", &solverMsgQKeys[i]);
    }

    fscanf(file, "%d", &numDocks);
    Dock docks[numDocks];

    for (int i = 0; i < numDocks; i++)
    {
        fscanf(file, "%d", &docks[i].category);
        for (int j = 0; j < docks[i].category; j++)
        {
            fscanf(file, "%d", &docks[i].craneCapacities[j]);
        }
        docks[i].assignedShip = -1;
        docks[i].remainingCargo = 0;
        docks[i].dockedShip = NULL;
        docks[i].id = i;
    }

    fclose(file);

    printf("Shared Memory Key: %d\n", sharedMemKey);
    printf("Main Message Queue Key: %d\n", mainMsgQKey);
    printf("Number of Solvers: %d\n", totalSolvers);
    for (int i = 0; i < totalSolvers; i++)
    {
        printf("Solver %d Message Queue Key: %d\n", i + 1, solverMsgQKeys[i]);
    }
    printf("Number of Docks: %d\n", numDocks);
    for (int i = 0; i < numDocks; i++)
    {
        printf("Dock %d Category: %d\n", i, docks[i].category);
        printf("Crane Capacities: ");
        for (int j = 0; j < docks[i].category; j++)
        {
            printf("%d ", docks[i].craneCapacities[j]);
        }
        printf("\n");
    }

    int id = msgget(mainMsgQKey, IPC_CREAT | 0666);
    if (id == -1)
    {
        perror("msgget");
        return EXIT_FAILURE;
    }
    MessageStruct msg;

    while (msgrcv(id, &msg, sizeof(msg), 0, 0) != -1)
    {
        switch (msg.mtype)
        {
        case 1:
            processNewShipRequests(sharedMemKey, msg, numDocks, docks, id, msg.timestep);
            break;
        default:
            printf("Unknown message type: %ld\n", msg.mtype);
            printf("Timestep: %d, Ship ID: %d, Direction: %d, Dock ID: %d\n", msg.timestep, msg.shipId, msg.direction, msg.dockId);
            printf("Is Finished: %d, Cargo ID: %d\n", msg.isFinished, msg.cargoId);
            break;
        }
    }

    return 0;
}