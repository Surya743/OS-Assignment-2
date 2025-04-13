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
#include <pthread.h>
#include <math.h>
#define MAX_CRANES 30
#define MAX_DOCKS 30
#define MAX_CARGO_COUNT 200
#define MAX_NEW_REQUESTS 100
#define MAX_AUTH_STRING_LEN 100

char charset[] = {'5', '6', '7', '8', '9', '.'};
char charsetred[] = {'5', '6', '7', '8', '9'};
int charsetSize = sizeof(charset) / sizeof(char);

typedef struct
{
    int id;
    int category;
    int craneCapacities[MAX_CRANES];
    int craneOriginalIndices[MAX_CRANES];
    int assignedShip;               //(-1 if empty)
    int remainingCargo;             // Remaining cargo to unload
    struct ShipRequest *dockedShip; // Pointer to the docked ship request
    int dockedTimestep;             // Timestep when the ship docked
    int lastCargoMovedTimestep;
    int dockedDirection;

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

Dock *findBestDock(ShipRequest *ship, int numDocks, Dock *docks)
{
    for (int i = 0; i < numDocks; i++)
    {
        if (docks[i].assignedShip == -1 && docks[i].category >= ship->category)
        {
            // printf("Found suitable dock %d for ship %d Assigned ship : %d\n ", docks[i].id, ship->shipId, docks[i].assignedShip);
            return &docks[i]; // First suitable dock in sorted list
        }
    }
    return NULL; // No suitable dock found
}

#define finalCharStartIdx 0

// Thread arguments structure
typedef struct
{
    int threadIndex;
    int totalThreads;
    int length;
    int dockId;
    int msgqid;
    char *authString;
    volatile int *found;
    pthread_mutex_t *lock;
} ThreadArgs;

void *solverThreadIterative(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    char guess[MAX_AUTH_STRING_LEN + 1];
    int maxLen = args->length;

    int charsetSize = sizeof(charset) / sizeof(char);
    int charsetRedSize = sizeof(charsetred) / sizeof(char);

    // Determine the number of positions that form the prefix (first and middle)
    int prefixLen = maxLen - 1; // last character will be iterated separately

    // We'll use an array "prefixDigits" of length prefixLen to hold the mixed-radix counter.
    // Position 0 uses charsetred, positions 1..prefixLen-1 use charset.
    int prefixDigits[prefixLen];
    
    // Initialize the prefix digits to the lowest value:
    // Position 0: first digit from charsetred
    prefixDigits[0] = 0;
    // Middle positions: first character from charset
    for (int i = 1; i < prefixLen; i++) {
        prefixDigits[i] = 0;
    }

    // Calculate total number of prefix combinations for this thread.
    // Total prefixes = charsetRedSize * (charsetSize^(prefixLen-1))
    long totalPrefixes = charsetRedSize;
    for (int i = 1; i < prefixLen; i++) {
        totalPrefixes *= charsetSize;
    }

    // Compute thread's range in the total prefix space.
    long prefixesPerThread = totalPrefixes / args->totalThreads;
    long startCount = prefixesPerThread * args->threadIndex;
    long endCount = (args->threadIndex == args->totalThreads - 1) ? totalPrefixes : (prefixesPerThread * (args->threadIndex + 1));

    // Fast forward to the starting prefix for this thread by "adding" startCount
    // This is done by simulating the mixed-radix addition.
    long remaining = startCount;
    for (int pos = prefixLen - 1; pos >= 0; pos--) {
        int base = (pos == 0) ? charsetRedSize : charsetSize;
        prefixDigits[pos] = remaining % base;
        remaining /= base;
    }

    // Main loop through assigned prefixes.
    for (long count = startCount; count < endCount && !*args->found; count++) {

        // Build the guess string from the current prefix
        // First character:
        guess[0] = charsetred[prefixDigits[0]];
        // Middle characters:
        for (int pos = 1; pos < prefixLen; pos++) {
            guess[pos] = charset[prefixDigits[pos]];
        }
        
        // Now loop through possible last characters (from charsetred)
        for (int i = 0; i < charsetRedSize && !*args->found; i++) {
            guess[maxLen - 1] = charsetred[i];
            guess[maxLen] = '\0';

            // Uncomment these if you need debugging output:
            // printf("guess is %s\n", guess);
            // usleep(500000);

            SolverRequest req = {.mtype = 2, .dockId = args->dockId};
            strncpy(req.authStringGuess, guess, MAX_AUTH_STRING_LEN);

            if (msgsnd(args->msgqid, &req, sizeof(req) - sizeof(long), 0) == -1)
            {
                perror("msgsnd");
                exit(EXIT_FAILURE);
            }

            SolverResponse resp;
            if (msgrcv(args->msgqid, &resp, sizeof(resp) - sizeof(long), 3, 0) == -1)
            {
                perror("msgrcv");
                exit(EXIT_FAILURE);
            }

            if (resp.guessIsCorrect)
            {
                pthread_mutex_lock(args->lock);
                if (!*args->found)
                {
                    *args->found = 1;
                    strncpy(args->authString, guess, MAX_AUTH_STRING_LEN);
                }
                pthread_mutex_unlock(args->lock);
                return NULL;
            }
        }
        
        // Increment the mixed-radix counter for the prefix.
        // Start at the rightmost position (prefixDigits[prefixLen-1])
        int pos = prefixLen - 1;
        while (pos >= 0) {
            int base = (pos == 0) ? charsetRedSize : charsetSize;
            prefixDigits[pos]++;
            if (prefixDigits[pos] < base)
                break;
            // Carry over:
            prefixDigits[pos] = 0;
            pos--;
        }
    }

    return NULL;
}

// Top-level function remains unchanged except for calling our updated solverThreadIterative.
void generateAndSendGuesses(int length, int dockId, char *authString, int *solverMsgQ, int numSolvers)
{
    // printf("The number of solvers is %d", numSolvers);
    SolverRequest preReq = {.mtype = 1, .dockId = dockId};
    usleep(300);

    int solverMsgIDs[numSolvers];
    for (int i = 0; i < numSolvers; i++)
    {
        int solver_q = msgget(solverMsgQ[i], IPC_CREAT | 0666);
        solverMsgIDs[i] = solver_q;
        if (msgsnd(solver_q, &preReq, sizeof(preReq) - sizeof(long), 0) == -1)
        {
            perror("msgsnd");
            exit(EXIT_FAILURE);
        }
    }

    pthread_t threads[numSolvers];
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    volatile int found = 0;
    ThreadArgs args[numSolvers];

    // Create threads for solving
    for (int i = 0; i < numSolvers; i++)
    {
        args[i] = (ThreadArgs){
            .threadIndex = i,
            .totalThreads = numSolvers,
            .length = length,
            .dockId = dockId,
            .msgqid = solverMsgIDs[i],
            .authString = authString,
            .found = &found,
            .lock = &lock};

        if (pthread_create(&threads[i], NULL, solverThreadIterative, &args[i]) != 0)
        {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < numSolvers; i++)
        pthread_join(threads[i], NULL);

    pthread_mutex_destroy(&lock);
}


void simulateCargoMovement(int numDocks, Dock *docks, int validationMsgQID, int currentTimestep, ShipRequest *shipsToUndock, MainSharedMemory *mainMem, int numSolvers, int solverMsgQ[])
{
    // printf("\n-- Simulating cargo movement for current timestep --\n");

    int countOfshipsToUndock = 0;
    for (int i = 0; i < numDocks; i++)
    {
        // printf("Dock %d: Ship ID Parked: %d at timestep %d \n", docks[i].id, docks[i].assignedShip, docks[i].dockedTimestep);
        if (docks[i].assignedShip != -1 && docks[i].dockedShip != NULL && docks[i].dockedTimestep < currentTimestep)
        {
            ShipRequest *ship = docks[i].dockedShip;
            // ////printf("Docked Ship ID %d at Dock %d with category %d\n", ship->shipId, docks[i].id, ship->category);
            // Track which cranes have been used in this timestep
            int cranesUsed[docks[i].category];
            for (int c = 0; c < docks[i].category; c++)
            {
                cranesUsed[c] = 0;
            }

            // For each cargo item, try to assign the first suitable unused crane
            for (int k = 0; k < ship->numCargo; k++)
            {
                if (ship->cargo[k] == -1)
                    continue; // Already moved

                int bestCrane = -1;

                for (int c = 0; c < docks[i].category; c++)
                {
                    if (!cranesUsed[c] && docks[i].craneCapacities[c] >= ship->cargo[k])
                    {
                        bestCrane = c;
                        break;
                    }
                }

                if (bestCrane != -1)
                {
                    int originalCraneIndex = docks[i].craneOriginalIndices[bestCrane];

                    // printf("Dock %d, Crane %d (cap %d) moves cargo item %d (weight %d) from Ship ID %d\n", docks[i].id, originalCraneIndex, docks[i].craneCapacities[bestCrane], k, ship->cargo[k], ship->shipId);

                    MessageStruct msg;
                    msg.mtype = 4;
                    msg.dockId = docks[i].id;
                    msg.shipId = ship->shipId;
                    msg.cargoId = k;
                    msg.direction = ship->direction;
                    msg.craneId = originalCraneIndex; // Send correct original crane index
                    usleep(300);

                    if (!msgsnd(validationMsgQID, &msg, sizeof(msg) - sizeof(long), 0))
                    {
                        // printf("Sent cargo movement message for Ship ID %d, Cargo ID %d to Dock %d\n", ship->shipId, k, i);
                    }
                    else
                    {
                        perror("Failed to send cargo movement message");
                        exit(EXIT_FAILURE);
                    }

                    ship->cargo[k] = -1; // Mark as moved
                    docks[i].remainingCargo--;
                    docks[i].lastCargoMovedTimestep = currentTimestep;
                    cranesUsed[bestCrane] = 1;
                }
            }

            // If all cargo is moved, undock the ship
            if (docks[i].remainingCargo <= 0 && docks[i].lastCargoMovedTimestep < currentTimestep)
            {
                // printf("Ship ID %d finished unloading at Dock %d and undocks.\n", ship->shipId, docks[i].id);
                docks[i].assignedShip = -1;
                docks[i].dockedShip = NULL;
                docks[i].remainingCargo = 0;

                // Create a copy of ship
                ShipRequest *shipCopy = malloc(sizeof(ShipRequest));
                *shipCopy = *ship;
                shipsToUndock[countOfshipsToUndock] = *shipCopy;
                countOfshipsToUndock++;

                int authStringLength = docks[i].lastCargoMovedTimestep - docks[i].dockedTimestep;
                char authString[MAX_AUTH_STRING_LEN];
                generateAndSendGuesses(authStringLength, docks[i].id, authString, solverMsgQ, numSolvers);
                // printf("Generated auth string for Ship ID %d: %s\n", ship->shipId, authString);

                // Remove \0 from authString

                strncpy(mainMem->authStrings[docks[i].id], authString, MAX_AUTH_STRING_LEN);

                MessageStruct msg;
                msg.mtype = 3;
                msg.dockId = docks[i].id;
                msg.shipId = ship->shipId;
                msg.direction = ship->direction;

                usleep(300);
                if (!msgsnd(validationMsgQID, &msg, sizeof(msg) - sizeof(long), 0))
                {
                    // printf("Sent undocking message for Ship ID %d to Dock %d\n", ship->shipId, docks[i].id);
                }
                else
                {
                    perror("Failed to send undocking message");
                    exit(EXIT_FAILURE);
                }
            }
        }
    }
}

ShipRequest *waitingShips[10000];
int waitingShipCount = 0;

void assignShipToDock(Dock *dock, ShipRequest *ship, int currentTimestep, int msgQID)
{
    dock->assignedShip = ship->shipId;
    dock->dockedShip = ship;
    dock->remainingCargo = ship->numCargo;
    dock->dockedDirection = ship->direction;
    dock->dockedTimestep = currentTimestep;

    // printf("Assigned Ship ID %d to Dock ID %d (Emergency: %d)\n", ship->shipId, dock->id, ship->emergency);

    MessageStruct msg = {
        .mtype = 2,
        .dockId = dock->id,
        .shipId = ship->shipId,
        .direction = ship->direction};
    usleep(300);
    if (msgsnd(msgQID, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1)
    {
        perror("Docking failed to send message");
        exit(EXIT_FAILURE);
    }
}

int compareQueueShips(const void *a, const void *b)
{
    const ShipRequest *shipA = *(const ShipRequest **)a;
    const ShipRequest *shipB = *(const ShipRequest **)b;

    if (shipA->emergency != shipB->emergency)
        return shipB->emergency - shipA->emergency;

    int priorityA = shipA->timestep + shipA->waitingTime;
    int priorityB = shipB->timestep + shipB->waitingTime;
    if (priorityA != priorityB)
        return priorityA - priorityB;

    return shipA->timestep - shipB->timestep;
}

void assignWaitingShips(int numDocks, Dock *docks, int validationMsgQID, int currentTimestep)
{
    // Sort all ships based on priority
    qsort(waitingShips, waitingShipCount, sizeof(ShipRequest *), compareQueueShips);

    // printf("---- All Sorted Ships ----\n");
    for (int i = 0; i < waitingShipCount; i++)
    {
        ShipRequest *s = waitingShips[i];
        // printf("Ship ID %d Dir %d Timestep %d Waiting %d Emergency %d Category %d \n", s->shipId, s->direction, s->timestep, s->waitingTime, s->emergency, s->category);
    }

    ShipRequest *newWaitingShips[10000];
    int remainingShips = 0;

    // ---- First Pass: Assign Emergency Ships Only ----
    for (int i = 0; i < waitingShipCount; i++)
    {
        ShipRequest *ship = waitingShips[i];
        if (!ship->emergency)
            continue;

        Dock *freeDock = findBestDock(ship, numDocks, docks);
        if (freeDock)
        {
            assignShipToDock(freeDock, ship, currentTimestep, validationMsgQID);
        }
        else
        {
            newWaitingShips[remainingShips++] = ship;
        }
    }

    // ---- Second Pass: Assign Non-Emergency Ships ----
    for (int i = 0; i < waitingShipCount; i++)
    {
        ShipRequest *ship = waitingShips[i];
        if (ship->emergency)
            continue;

        Dock *freeDock = findBestDock(ship, numDocks, docks);
        int canAssign = 0;

        if (freeDock)
        {
            if ((ship->direction == 1 && currentTimestep <= ship->timestep + ship->waitingTime) ||
                (ship->direction == -1))
            {
                canAssign = 1;
            }
        }

        if (canAssign)
        {
            assignShipToDock(freeDock, ship, currentTimestep, validationMsgQID);
        }
        else
        {
            newWaitingShips[remainingShips++] = ship;
        }
    }

    // Copy back remaining ships
    for (int i = 0; i < remainingShips; i++)
        waitingShips[i] = newWaitingShips[i];
    waitingShipCount = remainingShips;
}

void processNewShipRequests(int sharedMemKey, MessageStruct msg, int numDocks, Dock *docks, int validationMsgQID, int currentTimestep, int solverMsgQueues[], int numSolvers)
{
    // printf("\n== New timestep message received (Timestep: %d) ==\n", msg.timestep);

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
    // printf("Attached shared memory for new requests\n");

    for (int i = 0; i < msg.numShipRequests; i++)
    {
        ShipRequest *request = &mainMem->newShipRequests[i];
        ShipRequest *newRequest = malloc(sizeof(ShipRequest));
        memcpy(newRequest, request, sizeof(ShipRequest));

        if (newRequest->emergency)
            newRequest->waitingTime = -1;

        waitingShips[waitingShipCount++] = newRequest;
    }

    assignWaitingShips(numDocks, docks, validationMsgQID, currentTimestep);

    ShipRequest shipsToUndock[MAX_DOCKS];
    simulateCargoMovement(numDocks, docks, validationMsgQID, currentTimestep, shipsToUndock, mainMem, numSolvers, solverMsgQueues);

    // Tell validation that go to next timestep

    MessageStruct msgToValidation;
    msgToValidation.mtype = 5;
    usleep(300);

    if (!msgsnd(validationMsgQID, &msgToValidation, sizeof(msgToValidation) - sizeof(long), 0))
    {
        // printf("Sent message to validation to go to next timestep\n");
    }
    else
    {
        perror("Failed to send message to validation");
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Need testcase number as argument\n");
        return EXIT_FAILURE;
    }

    int testcase = atoi(argv[1]);

    char filename[20];
    snprintf(filename, sizeof(filename), "testcase%d/input.txt", testcase);
    FILE *file = fopen(filename, "r");
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
            docks[i].craneOriginalIndices[j] = j; // Save original index
        }
        docks[i].assignedShip = -1;
        docks[i].remainingCargo = 0;
        docks[i].dockedShip = NULL;
        docks[i].id = i;
    }

    // Sort cranes inside each dock based on capacity, but keep original indices
    for (int i = 0; i < numDocks; i++)
    {
        for (int j = 0; j < docks[i].category - 1; j++)
        {
            for (int k = j + 1; k < docks[i].category; k++)
            {
                if (docks[i].craneCapacities[j] > docks[i].craneCapacities[k])
                {
                    // Swap capacities
                    int tempCap = docks[i].craneCapacities[j];
                    docks[i].craneCapacities[j] = docks[i].craneCapacities[k];
                    docks[i].craneCapacities[k] = tempCap;

                    // Swap original indices too
                    int tempIdx = docks[i].craneOriginalIndices[j];
                    docks[i].craneOriginalIndices[j] = docks[i].craneOriginalIndices[k];
                    docks[i].craneOriginalIndices[k] = tempIdx;
                }
            }
        }
    }

    // Sort docks based on category (ascending)
    for (int i = 0; i < numDocks - 1; i++)
    {
        for (int j = i + 1; j < numDocks; j++)
        {
            if (docks[i].category > docks[j].category)
            {
                Dock temp = docks[i];
                docks[i] = docks[j];
                docks[j] = temp;
            }
        }
    }

    // Print docks

    for (int i = 0; i < numDocks; i++)
    {
        // printf("Dock %d: Category %d, Cranes: ", docks[i].id, docks[i].category);
        for (int j = 0; j < docks[i].category; j++)
        {
            // printf("%d ", docks[i].craneCapacities[j]);
        }
        // printf("\n");
    }

    // exit(0);

    fclose(file);

    // printf("Shared Memory Key: %d\n", sharedMemKey);
    // printf("Main Message Queue Key: %d\n", mainMsgQKey);
    // printf("Number of Solvers: %d\n", totalSolvers);
    for (int i = 0; i < totalSolvers; i++)
    {
        // printf("Solver %d Message Queue Key: %d\n", i + 1, solverMsgQKeys[i]);
    }
    // printf("Number of Docks: %d\n", numDocks);
    for (int i = 0; i < numDocks; i++)
    {
        // printf("Dock %d Category: %d\n", i, docks[i].category);
        // printf("Crane Capacities: ");
        for (int j = 0; j < docks[i].category; j++)
        {
            // printf("%d ", docks[i].craneCapacities[j]);
        }
        // printf("\n");
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
            if (msg.isFinished)
            {
                // printf("Scheduler docked all ships\n");
                exit(0);
            }
            processNewShipRequests(sharedMemKey, msg, numDocks, docks, id, msg.timestep, solverMsgQKeys, totalSolvers);
            break;
        default:
            // printf("Unknown message type: %ld\n", msg.mtype);
            // printf("Timestep: %d, Ship ID: %d, Direction: %d, Dock ID: %d\n", msg.timestep, msg.shipId, msg.direction, msg.dockId);
            // printf("Is Finished: %d, Cargo ID: %d\n", msg.isFinished, msg.cargoId);
            break;
        }
    }

    return 0;
}