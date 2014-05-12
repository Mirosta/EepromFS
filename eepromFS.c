#include <avr/eeprom.h>
#include "debug.h"
const uint8_t INUSE = 0;
const uint8_t ISLEN = 128;
const uint8_t NULLPTR = 255;
const uint8_t MAXFILES = 61;
const uint8_t FILEREAD = 2;
const uint8_t FILEWRITE = 1;
const uint8_t FILECLOSED = 0;

const int16_t FILEALREADYOPEN = -1;
const int16_t FILEDOESNTEXIST = -2;
const int16_t WRONGFILEIOTYPE = -3;
const int16_t INVALIDACCESSHANDLE = -4;
const int16_t OUTOFSPACE = -5;

const uint16_t BLOCKSIZE = 32;
const uint16_t NEXTBLOCKPTR = 31;
const uint16_t EEPROMSIZE = 2048;
const uint16_t BLOCKINDEX = 1;
const uint16_t CONFIGUREINDEX = 0;
const uint16_t FILEMEMORYAVAILABLE = 2048-64-32;
const uint16_t FILEINDEX = 32+64;
#define NUMBLOCKBYTES ((2048-64-32)/32/8)
#define NUMFILEPTRBYTES ((2048-64-32)/32)
#define true 1
#define false 0
/*
The first 1 byte is a configured byte which is 0x1A XOR NUMBLOCKBYTES
The next 8 bytes is the block states each bit represents 1 block, 0 = in use, 1 = available
The next 61 bytes is the start block for each file
Each block contains 31 bytes of data
The last byte is either a pointer to the next block or the length of the data in the block
If bit 7 is set it's a length field otherwise it's a block pointer
If it's 255 it's an empty block
Each block is 32 bytes

*/

uint16_t FILEPTRINDEX;

uint8_t blocks[NUMBLOCKBYTES];
uint8_t filePtrs[NUMFILEPTRBYTES];

uint8_t fileAccess[NUMFILEPTRBYTES];
uint8_t filePosition[NUMFILEPTRBYTES];
uint8_t fileBlock[NUMFILEPTRBYTES];

#define shouldWrite 1

void setupEeprom();
void initEepfs();
void setBlockPtr(uint8_t, uint8_t);

void initEepfs()
{
    FILEPTRINDEX = BLOCKINDEX + NUMBLOCKBYTES;

    uint8_t configured = eeprom_read_byte(CONFIGUREINDEX);
    printf("Read configured: %X\n", configured);
    if(configured != (NUMBLOCKBYTES^0xA1))
    {
        printf("Setting up EEPROM\n");
        setupEeprom();
    }
    else
    {
        printf("Reading block states\n");
        eeprom_read_block((void*)blocks, (const void*)BLOCKINDEX, NUMBLOCKBYTES);
        printf("Reading file ptrs\n");
        eeprom_read_block((void*)filePtrs, (const void*)FILEPTRINDEX, NUMFILEPTRBYTES);
    }

    uint8_t b;
    for(b = 0; b < NUMBLOCKBYTES; b++)
    {
        printf("%X ", blocks[b]);
    }
    printf("\n");
    uint16_t i;
    for(i = 0; i < NUMFILEPTRBYTES; i++)
    {
        fileAccess[i] = FILECLOSED;
        filePosition[i] = 0;
        fileBlock[i] = NULLPTR;
    }
    printf("Setup default file access\n");
}

void setupEeprom()
{
    eeprom_update_byte(CONFIGUREINDEX, NUMBLOCKBYTES^0xA1);

    int i;
    for(i = 0; i < NUMBLOCKBYTES; i++)
    {
        blocks[i] = 255;
    }
    for(i = 0; i < NUMFILEPTRBYTES; i++)
    {
        filePtrs[i] = 255;
    }

    if(shouldWrite) eeprom_update_block((void*)BLOCKINDEX, (const void*)blocks, NUMBLOCKBYTES);
    if(shouldWrite) eeprom_update_block((void*)FILEPTRINDEX, (const void*)filePtrs, NUMFILEPTRBYTES);
}

//Is a block in use
uint8_t isInUse(uint8_t block)
{
    uint8_t index = block >> 3; //Divide by 8
    uint8_t bit = block & 7; //mod 8
    return (blocks[index] & (uint8_t)(1 << bit)) == INUSE;
}

//Marks a block as used or not used
void setInUse(uint8_t block, uint8_t inUse)
{
    uint8_t index = block >> 3; //Divide by 8
    uint8_t bit = block & 7; //mod 8
    if(inUse == false)
    {
        blocks[index] |= (uint8_t)_BV(bit);
    }
    else
    {
        blocks[index] &= (uint8_t)(~_BV(bit));
    }
    if(shouldWrite) eeprom_update_byte(BLOCKINDEX+index, blocks[index]);
}

//Check if a file exists
uint8_t fileExists(uint8_t file)
{
    return filePtrs[file] != NULLPTR;
}

//Find the next empty block
uint8_t getEmptyBlock()
{
    uint8_t blockNum;

    for(blockNum = 0; blockNum < NUMBLOCKBYTES*8; blockNum++)
    {
        if(!isInUse(blockNum)) return blockNum;
    }

    return NULLPTR;
}

//Sets the start block for a file
void setFilePtr(uint8_t file, uint8_t block)
{
    filePtrs[file] = block;
    if(shouldWrite) eeprom_update_byte(FILEPTRINDEX+file, filePtrs[file]);
}

//Gets an empty block to start the file in
//Sets the file's start block to that block
//Marks the blocks as in use
//Changes the block ptr to null
uint8_t createNewFile(uint8_t file)
{
    uint8_t startBlock = getEmptyBlock();
    if(startBlock == NULLPTR) return NULLPTR;
    setFilePtr(file, startBlock);
    setInUse(startBlock, true);
    setBlockPtr(startBlock, NULLPTR);

    return 0;
}

uint8_t getBlockPtr(uint8_t block)
{
    return eeprom_read_byte(FILEINDEX + block*BLOCKSIZE + NEXTBLOCKPTR);
}

void setBlockPtr(uint8_t block, uint8_t ptr)
{
    if(shouldWrite) eeprom_update_byte(FILEINDEX + block*BLOCKSIZE + NEXTBLOCKPTR, ptr);
}

void wipeFile(uint8_t file)
{
    uint8_t startBlock = filePtrs[file];
    if(startBlock == NULLPTR) return;
    uint8_t nextPointer = getBlockPtr(startBlock);
    while((nextPointer & ISLEN)!= ISLEN)
    {
        setInUse(nextPointer, false);
        nextPointer = getBlockPtr(nextPointer);
    }
    setBlockPtr(startBlock, NULLPTR);
}

//Fails if file is already open or no space left
//Creates or wipes the file
//Resets the file's position
//Sets the file's current block to the start block
//Sets the access type to WRITE
int16_t open_for_write(uint8_t file)
{
    if(fileAccess[file] != FILECLOSED) return FILEALREADYOPEN;
    if(filePtrs[file] == NULLPTR) createNewFile(file);
    if(filePtrs[file] == NULLPTR) return OUTOFSPACE;
    wipeFile(file);
    filePosition[file] = 0;
    fileBlock[file] = filePtrs[file];
    fileAccess[file] = FILEWRITE;
    return file;
}

//Runs from the starting block to the end
//moves the position to the end of the file
//sets the current block to the last block
void fastForward(uint8_t file)
{
    uint8_t startBlock = filePtrs[file];
    if(startBlock == NULLPTR) return;
    uint8_t nextPointer = getBlockPtr(startBlock);
    while((nextPointer & ISLEN) != ISLEN)
    {
        filePosition[file] += BLOCKSIZE;
        fileBlock[file] = nextPointer;
        nextPointer = getBlockPtr(nextPointer);
    }
    if(nextPointer != NULLPTR) filePosition[file] += nextPointer & (~ISLEN);
}

//Creates or appends a file
//Sets the access to write
//Fails if the file is already open or out of space
int16_t open_for_append(uint8_t file)
{
    if(fileAccess[file] != FILECLOSED) return FILEALREADYOPEN;
    if(filePtrs[file] == NULLPTR) createNewFile(file);
    if(filePtrs[file] == NULLPTR) return OUTOFSPACE;
    fastForward(file);
    fileAccess[file] = FILEWRITE;
    return file;
}

//Opens an existing file
//Sets the position to 0
//Sets the current block to the start block
//Sets the access to READ
//Fails if the file is already open
//Fails if the file doesn't exist

int16_t open_for_read(uint8_t file)
{
    if(fileAccess[file] != FILECLOSED) return FILEALREADYOPEN;
    if(filePtrs[file] == NULLPTR) return FILEDOESNTEXIST;
    filePosition[file] = 0;
    fileBlock[file] = filePtrs[file];
    fileAccess[file] = FILEREAD;
    return file;
}

//Updates the last block with the current length of data stored in it
void writeBlockLength(uint8_t file)
{
    uint8_t currentBlock = fileBlock[file];
    uint8_t currentLength = filePosition[file] & 31; //Mod 32
    if(currentBlock == NULLPTR) return;
    printf("Writing block length: %X to block %u\n", (currentLength | ISLEN), currentBlock);
    setBlockPtr(currentBlock, currentLength|ISLEN);
}

//Closes a file
//Returns instantly if it's already closed
//Updates the last block length
//Sets the current block to null
//Sets the access to closed
//Fails if the file doesn't exist
int16_t close(uint8_t file)
{
    if(filePtrs[file] == NULLPTR) return FILEDOESNTEXIST;
    if(fileAccess[file] == FILECLOSED) return 0;
    writeBlockLength(file);
    fileBlock[file] = NULLPTR;
    fileAccess[file] = FILECLOSED;
    return 0;
}

//
int16_t write(int16_t accessHandle, const void *buffer, uint16_t size)
{
    if(accessHandle < 0 || accessHandle > NUMFILEPTRBYTES) return INVALIDACCESSHANDLE;
    if(fileAccess[accessHandle] != FILEWRITE) return WRONGFILEIOTYPE;

    const uint8_t *byteBuffer = (const uint8_t*)buffer;
    uint16_t i = 0;
    while(i < size)
    {
        uint8_t block = fileBlock[accessHandle]; //Current block
        uint8_t blockOffset = filePosition[accessHandle] & 31;//Mod 32
        uint16_t remaining = size-i;
        void *bufferPtr = byteBuffer+i; //Offset pointer into the buffer
        printf("AmWritten: %u\n", i);
        printf("BufferPtr: %p\n", bufferPtr);
        if(remaining + blockOffset > BLOCKSIZE-1) //More than the current block of data left
        {
            if(shouldWrite) printf("Writing %u bytes to files %u at %u\n", BLOCKSIZE-blockOffset-1, accessHandle, FILEINDEX + block*BLOCKSIZE + blockOffset);
            if(shouldWrite) eeprom_update_block(bufferPtr, (void *)(FILEINDEX + block*BLOCKSIZE + blockOffset), BLOCKSIZE-blockOffset-1); //Fill the remainder of the block

            uint8_t nextBlock = getEmptyBlock(); //Get a new block
            if(nextBlock == NULLPTR) return OUTOFSPACE; //If we can't find a new block return
            setBlockPtr(block, nextBlock); //Set the pointer of the current block to the new block
            setBlockPtr(nextBlock, NULLPTR); //Set the pointer of the new block to null
            setInUse(nextBlock, true);//Mark the block as in use
            fileBlock[accessHandle] = nextBlock; //Set the current block as the new block
            i+= BLOCKSIZE-blockOffset-1; //Increase the data written by the correct amount
            filePosition[accessHandle] += BLOCKSIZE-blockOffset; //Increase the file position
        }
        else //Less than or exactly enough to fill the current block
        {
            if(shouldWrite) printf("Writing %u bytes to files %u at %u\n", remaining, accessHandle, FILEINDEX + block*BLOCKSIZE + blockOffset);
            if(shouldWrite) eeprom_update_block(bufferPtr, (void *)(FILEINDEX + block*BLOCKSIZE + blockOffset), remaining); //Write the values into the block

            i+= remaining; //Increase the data written
            filePosition[accessHandle] += remaining; //Increase the file position
        }
    }
    return 0; //SUCCESS
}

int16_t read(int16_t accessHandle, void* buffer, uint16_t size)
{
    if(accessHandle < 0 || accessHandle > NUMFILEPTRBYTES) return INVALIDACCESSHANDLE;
    if(fileAccess[accessHandle] != FILEREAD) return WRONGFILEIOTYPE;

    int16_t amRead = 0;
    uint8_t *byteBuffer = (uint8_t*)buffer;

    while(amRead < size)
    {
        uint8_t block = fileBlock[accessHandle]; //Current block
        uint8_t blockOffset = filePosition[accessHandle] & 31;//Mod 32
        uint16_t remaining = size-amRead;
        void *bufferPtr = byteBuffer+amRead; //Offset pointer into the buffer
        uint8_t nextBlock = getBlockPtr(block); //The next block pointer
        uint8_t blockLen = BLOCKSIZE-1; //Max amount of data available in the current block
        uint8_t isEnd = (nextBlock & ISLEN) == ISLEN; //Check if this block is the last block
        if(isEnd && nextBlock != NULLPTR) blockLen = nextBlock & (~ISLEN); //If it's the last block change the max amount of data to the length stored in the ptr
        printf("Am read: %u\n", amRead);

        if(remaining + blockOffset > blockLen && blockLen-blockOffset > 0) //More than just this block to read where amount left to read in this block is > 0
        {
            eeprom_read_block(bufferPtr, (const void*)(FILEINDEX + block*BLOCKSIZE + blockOffset), blockLen-blockOffset);
            amRead += blockLen-blockOffset;
            filePosition[accessHandle] += blockLen-blockOffset;
        }
        else if(blockLen-blockOffset > 0) //This block or less left to read and amount left to read > 0
        {
            eeprom_read_block(bufferPtr, (const void*)(FILEINDEX + block*BLOCKSIZE + blockOffset), remaining);
            amRead += remaining;
            filePosition[accessHandle] += remaining;
        }
        if(isEnd) return amRead; //Return the actual amount read, stop reading anymore
        else if(remaining > blockLen) //Else set the current block to the next block
        {
            fileBlock[accessHandle] = nextBlock;
            filePosition[accessHandle] ++; //Skip blockPtr
        }
    }

    return amRead; //Return the amount read
}

//Wipe the file
//Mark the start block as free
//Set the file ptr to null
int16_t delete_file(uint8_t file)
{
    if(filePtrs[file] == NULLPTR) return FILEDOESNTEXIST;
    if(fileAccess[file] != FILECLOSED) return FILEALREADYOPEN;
    wipeFile(file);
    setInUse(filePtrs[file], false);
    setFilePtr(file, NULLPTR);
    return 0;
}

void printError(int16_t errorcode)
{
    if(errorcode >= 0) return;
    else
    {
        switch(errorcode)
        {
            case -1://FILEALREADYOPEN:
                printf("File already open\n");
            break;
            case -2://FILEDOESNTEXIST:
                printf("File doesn't exist\n");
            break;
            case -3://WRONGFILEIOTYPE:
                printf("Tried to read/write on file opened for write/read\n");
            break;
            case -4://INVALIDACCESSHANDLE:
                printf("Invalid access handle\n");
            break;
            case -5://OUTOFSPACE:
                printf("EEPROM out of space\n");
            break;
            default:
                printf("Unknown errorcode: %d", errorcode);
            break;
        }
    }
}

void init_processor()
{

}

int main(void)
{
    init_debug_uart1();
    initEepfs();
    init_processor();

    if(fileExists(0)) printf("File 0 exists\n");
    else printf("File 0 doesn't exist\n");

    int16_t fileWrite = open_for_write(0);
    if(fileWrite < 0)
    {
        printError(fileWrite);
    }
    else
    {
        printf("Opened file 0\n");
        uint8_t data[64];
        uint8_t i;
        for(i = 0; i < 64; i++) data[i] = i+1;

        write(fileWrite, (const void *)&data, 64);
        printf("Wrote 1-5 into file 0\n");
        close(fileWrite);
        printf("Closed file 0\n");
    }
    int16_t fileRead = open_for_read(0);
    if(fileRead < 0)
    {
        printError(fileRead);
    }
    else
    {
        printf("Opened file 0\n");
        uint8_t data[65];
        uint8_t i;
        for(i = 0; i < 65; i++) data[i] = 0;

        printf("Trying to read 6 bytes from file 0\n");
        int16_t amRead = read(fileRead, (void *)&data, 65);
        if(amRead < 0) printError(amRead);
        else
        {
            printf("Read %d bytes from file 0\n", amRead);
            int16_t i;
            for(i = 0; i < amRead; i++)
            {
                printf("%u ", data[i]);
            }
            printf("\n");
        }
    }
    while(1)
    {

    }
}
