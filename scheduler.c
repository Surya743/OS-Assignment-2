#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>

#define MAX_CRANES 30
#define MAX_DOCKS 30
#define MAX_CARGO_COUNT 200
#define MAX_NEW_REQUESTS 50
#define MAX_AUTH_STRING_LEN 100

typedef struct
{
    int category;
    int craneCapacities[MAX_CRANES];
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
    int totalSolvers, num_docks;

    fscanf(file, "%d", &sharedMemKey);
    fscanf(file, "%d", &mainMsgQKey);

    fscanf(file, "%d", &totalSolvers);
    int solverMsgQKeys[totalSolvers];
    for (int i = 0; i < totalSolvers; i++)
    {
        fscanf(file, "%d", &solverMsgQKeys[i]);
    }

    fscanf(file, "%d", &num_docks);
    Dock docks[num_docks];

    for (int i = 0; i < num_docks; i++)
    {
        fscanf(file, "%d", &docks[i].category);
        for (int j = 0; j < docks[i].category; j++)
        {
            fscanf(file, "%d", &docks[i].craneCapacities[j]);
        }
    }

    fclose(file);

    printf("Shared Memory Key: %d\n", sharedMemKey);
    printf("Main Message Queue Key: %d\n", mainMsgQKey);
    printf("Number of Solvers: %d\n", totalSolvers);
    for (int i = 0; i < totalSolvers; i++)
    {
        printf("Solver %d Message Queue Key: %d\n", i + 1, solverMsgQKeys[i]);
    }
    printf("Number of Docks: %d\n", num_docks);
    for (int i = 0; i < num_docks; i++)
    {
        printf("Dock %d Category: %d\n", i + 1, docks[i].category);
        printf("Crane Capacities: ");
        for (int j = 0; j < docks[i].category; j++)
        {
            printf("%d ", docks[i].craneCapacities[j]);
        }
        printf("\n");
    }

    int key = msgget(mainMsgQKey, IPC_CREAT | 0666);
    if (key == -1)
    {
        perror("msgget");
        return EXIT_FAILURE;
    }
    MessageStruct msg;
    if (!msgrcv(key, &msg, sizeof(msg), 1, 0))
    {
        perror("msgrcv");
        return EXIT_FAILURE;
    }

    printf("Received message from validation: %d new ship requests\n", msg.numShipRequests);

    int id = shmget(sharedMemKey, sizeof(MainSharedMemory), IPC_CREAT | 0666);
    if (id == -1)
    {
        perror("shmget");
        return EXIT_FAILURE;
    }

    MainSharedMemory *mainMem = (MainSharedMemory *)shmat(id, NULL, 0);
    if (mainMem == (void *)-1)
    {
        perror("shmat");
        return EXIT_FAILURE;
    }
    printf("Attached shared memory\n");
    // Print the new ship requests
    for (int i = 0; i < msg.numShipRequests; i++)
    {
        ShipRequest *request = &mainMem->newShipRequests[i];
        printf("Ship ID: %d, Timestep: %d, Category: %d, Direction: %d, Emergency: %d, Waiting Time: %d, Number of Cargo: %d\n",
               request->shipId, request->timestep, request->category, request->direction,
               request->emergency, request->waitingTime, request->numCargo);
        for (int j = 0; j < request->numCargo; j++)
        {
            printf("Cargo ID: %d\n", request->cargo[j]);
        }
    }
    return 0;
}