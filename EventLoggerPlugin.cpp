//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose: 
//
// $NoKeywords: $
//
//===========================================================================//

#include <stdio.h>

#include "interface.h"
#include "filesystem.h"
#include "engine/iserverplugin.h"
#include "game/server/iplayerinfo.h"
#include "eiface.h"
#include "igameevents.h"
#include "convar.h"
#include "Color.h"
#include "vstdlib/random.h"
#include "engine/IEngineTrace.h"
#include "tier2/tier2.h"

#include "libpq-fe.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// #define DB_CONNECT_STR "host=127.0.0.1 port=5432 dbname=tfstats user=tfstats password=fakepassword"
#if !defined(DB_CONNECT_STR)
#error "Define DB_CONNECT_STR with PostgreSQL database connection information, eg. \"host=127.0.0.1 dbname=tfstats user=tfstats password=...\""
#endif

// Interfaces from the engine
IVEngineServer	*engine = NULL; // helper functions (messaging clients, loading content, making entities, running commands, etc)
IGameEventManager *gameeventmanager = NULL; // game events interface
IPlayerInfoManager *playerinfomanager = NULL; // game dll interface to interact with players
IBotManager *botmanager = NULL; // game dll interface to interact with bots
IServerPluginHelpers *helpers = NULL; // special 3rd party plugin helpers from the engine
IUniformRandomStream *randomStr = NULL;
IEngineTrace *enginetrace = NULL;

CGlobalVars *gpGlobals = NULL;

//---------------------------------------------------------------------------------
// Purpose: a sample 3rd party plugin class
//---------------------------------------------------------------------------------
class CEventLoggerPlugin : public IServerPluginCallbacks, public IGameEventListener
{
public:
    CEventLoggerPlugin();
    ~CEventLoggerPlugin();

    // IServerPluginCallbacks methods
    virtual bool			Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory);
    virtual void			Unload(void);
    virtual void			Pause(void);
    virtual void			UnPause(void);
    virtual const char     *GetPluginDescription(void);      
    virtual void			LevelInit(char const *pMapName );
    virtual void			ServerActivate(edict_t *pEdictList, int edictCount, int clientMax);
    virtual void			GameFrame(bool simulating);
    virtual void			LevelShutdown(void);
    virtual void			ClientActive(edict_t *pEntity);
    virtual void			ClientDisconnect(edict_t *pEntity);
    virtual void			ClientPutInServer(edict_t *pEntity, char const *playername);
    virtual void			SetCommandClient(int index);
    virtual void			ClientSettingsChanged(edict_t *pEdict);
    virtual PLUGIN_RESULT	ClientConnect(bool *bAllowConnect, edict_t *pEntity, const char *pszName, const char *pszAddress, char *reject, int maxrejectlen);
    virtual PLUGIN_RESULT	ClientCommand(edict_t *pEntity, const CCommand &args);
    virtual PLUGIN_RESULT	NetworkIDValidated(const char *pszUserName, const char *pszNetworkID);
    virtual void			OnQueryCvarValueFinished(QueryCvarCookie_t iCookie, edict_t *pPlayerEntity, EQueryCvarValueStatus eStatus, const char *pCvarName, const char *pCvarValue);

    // IGameEventListener Interface
    virtual void FireGameEvent(KeyValues * event);

    virtual int GetCommandIndex() { return m_iClientCommandIndex; }

private:
    void LogEvent(KeyValues* event);
    void DatabaseConnect();

    int m_iClientCommandIndex;
    char* m_gameSessionId;
    PGconn* m_db;
    int m_frameCounter;
};


// 
// The plugin is a static singleton that is exported as an interface
//
CEventLoggerPlugin g_EmtpyServerPlugin;
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CEventLoggerPlugin, IServerPluginCallbacks, INTERFACEVERSION_ISERVERPLUGINCALLBACKS, g_EmtpyServerPlugin);

//---------------------------------------------------------------------------------
// Purpose: constructor/destructor
//---------------------------------------------------------------------------------
CEventLoggerPlugin::CEventLoggerPlugin()
{
    m_iClientCommandIndex = 0;
    m_db = NULL;
    m_gameSessionId = NULL;
    m_frameCounter = 0;
}

CEventLoggerPlugin::~CEventLoggerPlugin()
{
}

//---------------------------------------------------------------------------------
// Purpose: called when the plugin is loaded, load the interface we need from the engine
//---------------------------------------------------------------------------------
bool CEventLoggerPlugin::Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory)
{
    ConnectTier1Libraries(&interfaceFactory, 1);
    ConnectTier2Libraries(&interfaceFactory, 1);

    playerinfomanager = (IPlayerInfoManager *)gameServerFactory(INTERFACEVERSION_PLAYERINFOMANAGER,NULL);
    if (!playerinfomanager)
    {
        Warning("Unable to load playerinfomanager, ignoring\n"); // this isn't fatal, we just won't be able to access specific player data
    }

    botmanager = (IBotManager*)gameServerFactory(INTERFACEVERSION_PLAYERBOTMANAGER, NULL);
    if (!botmanager)
    {
        Warning("Unable to load botcontroller, ignoring\n"); // this isn't fatal, we just won't be able to access specific bot functions
    }

    engine = (IVEngineServer*)interfaceFactory(INTERFACEVERSION_VENGINESERVER, NULL);
    gameeventmanager = (IGameEventManager *)interfaceFactory(INTERFACEVERSION_GAMEEVENTSMANAGER,NULL);
    helpers = (IServerPluginHelpers*)interfaceFactory(INTERFACEVERSION_ISERVERPLUGINHELPERS, NULL);
    enginetrace = (IEngineTrace *)interfaceFactory(INTERFACEVERSION_ENGINETRACE_SERVER,NULL);
    randomStr = (IUniformRandomStream *)interfaceFactory(VENGINE_SERVER_RANDOM_INTERFACE_VERSION, NULL);

    // get the interfaces we want to use
    if (!(engine && gameeventmanager && g_pFullFileSystem && helpers && enginetrace && randomStr))
    {
        return false; // we require all these interface to function
    }

    if (playerinfomanager)
    {
        gpGlobals = playerinfomanager->GetGlobalVars();
    }

    MathLib_Init(2.2f, 2.2f, 0.0f, 2.0f);
    ConVar_Register(0);
    DatabaseConnect();

    KeyValues* event = new KeyValues("_plugin_load");
    LogEvent(event);
    event->deleteThis();

    gameeventmanager->AddListener(this, true);

    return true;
}

void CEventLoggerPlugin::DatabaseConnect()
{
    if (m_db != NULL)
    {
        PQfinish(m_db);
        m_db = NULL;
    }
    if (m_gameSessionId != NULL)
    {
        free(m_gameSessionId);
        m_gameSessionId = NULL;
    }

    Msg("Connecting to stats database...\n");
    m_db = PQconnectdb(DB_CONNECT_STR);
    if (PQstatus(m_db) != CONNECTION_OK)
    {
        Warning("Failed to connect to stats database: %s\n", PQerrorMessage(m_db));
        PQfinish(m_db);
        m_db = NULL;
    }
    else
    {
        Msg("Successfully connected to stats database.\n");

        PGresult* res = PQexec(m_db, "INSERT INTO GameSession (Heartbeat) VALUES (NOW()) RETURNING Id");
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            Warning("\"INSERT INTO GameSession\" failed\n");
            PQclear(res);
            PQfinish(m_db);
            m_db = NULL;
        }
        else
        {
            m_gameSessionId = strdup(PQgetvalue(res, 0, 0));
            PQclear(res);

            KeyValues* event = new KeyValues("_new_gamesession");
            CGlobalVars* globals = playerinfomanager->GetGlobalVars();
            if (globals != NULL)
            {
                const char* mapname = globals->mapname.ToCStr();
                if (mapname != NULL && strlen(mapname) != 0)
                    event->SetString("map_name", mapname);
            }
            LogEvent(event);
            event->deleteThis();

            for (int i = 1; i <= globals->maxClients; i++)
            {
                edict_t* entity = engine->PEntityOfEntIndex(i);
                if (!entity || entity->IsFree())
                    continue;

                IPlayerInfo* player = playerinfomanager->GetPlayerInfo(entity);
                if (player != NULL)
                {
                    KeyValues* event = new KeyValues("_existing_client");
                    event->SetString("player_name", player->GetName());
                    event->SetInt("userid", player->GetUserID());
                    event->SetInt("team", player->GetTeamIndex());
                    const char* networkId = player->GetNetworkIDString();
                    if (networkId != NULL)
                        event->SetString("networkid", networkId);
                    event->SetInt("health", player->GetHealth());
                    // FIXME: player class for TF2?
                    LogEvent(event);
                    event->deleteThis();
                }
            }
        }
    }
}

//---------------------------------------------------------------------------------
// Purpose: called when the plugin is unloaded (turned off)
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::Unload(void)
{
    gameeventmanager->RemoveListener(this); // make sure we are unloaded from the event system

    ConVar_Unregister();
    DisconnectTier2Libraries();
    DisconnectTier1Libraries();

    KeyValues* event = new KeyValues("_plugin_unload");
    LogEvent(event);
    event->deleteThis();

    if (m_db != NULL)
    {
        PQfinish(m_db);
        m_db = NULL;
    }
    if (m_gameSessionId != NULL)
    {
        free(m_gameSessionId);
        m_gameSessionId = NULL;
    }
}

//---------------------------------------------------------------------------------
// Purpose: called when the plugin is paused (i.e should stop running but isn't unloaded)
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::Pause(void)
{
}

//---------------------------------------------------------------------------------
// Purpose: called when the plugin is unpaused (i.e should start executing again)
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::UnPause(void)
{
}

//---------------------------------------------------------------------------------
// Purpose: the name of this plugin, returned in "plugin_print" command
//---------------------------------------------------------------------------------
const char *CEventLoggerPlugin::GetPluginDescription(void)
{
    return "EventLoggerPlugin, Replicon Internal";
}

//---------------------------------------------------------------------------------
// Purpose: called on level start
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::LevelInit( char const *pMapName )
{
    gameeventmanager->AddListener(this, true);

    KeyValues* event = new KeyValues("_level_init", "map_name", pMapName);
    LogEvent(event);
    event->deleteThis();
}

//---------------------------------------------------------------------------------
// Purpose: called on level start, when the server is ready to accept client connections
//		edictCount is the number of entities in the level, clientMax is the max client count
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::ServerActivate(edict_t *pEdictList, int edictCount, int clientMax)
{
    char gameDir[512];
    engine->GetGameDir(gameDir, 512);

    KeyValues* event = new KeyValues("_server_activate");
    event->SetInt("client_max", clientMax);
    event->SetInt("app_id", engine->GetAppID());
    event->SetString("game_dir", gameDir);
    LogEvent(event);
    event->deleteThis();
}

//---------------------------------------------------------------------------------
// Purpose: called once per server frame, do recurring work here (like checking for timeouts)
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::GameFrame(bool simulating)
{
    if (simulating)
    {
        if (++m_frameCounter == 1800)   // 30s * 60 frames/sec
        {
            m_frameCounter = 0;

            if (m_db == NULL || PQstatus(m_db) != CONNECTION_OK)
                DatabaseConnect();

            if (m_db != NULL && PQstatus(m_db) == CONNECTION_OK)
            {
                const Oid paramTypes[] = { 23, };
                const char* const values[] = { m_gameSessionId };
                const int lengths[] = { strlen(m_gameSessionId) };
                const int paramFormats[] = { 0, 0, 0 };
                PGresult* res = PQexecParams(m_db, "UPDATE GameSession SET Heartbeat = NOW() WHERE Id = $1", 1, paramTypes, values, lengths, paramFormats, 0);
                ExecStatusType resStatus = PQresultStatus(res);
                PQclear(res);
                if (resStatus != PGRES_COMMAND_OK)
                    Warning("\"UPDATE GameSession SET Heartbeat\" failed\n");
            }
        }
    }
}

//---------------------------------------------------------------------------------
// Purpose: called on level end (as the server is shutting down or going to a new map)
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::LevelShutdown(void) // !!!!this can get called multiple times per map change
{
    gameeventmanager->RemoveListener(this);

    KeyValues* event = new KeyValues("_level_shutdown");
    LogEvent(event);
    event->deleteThis();
}

//---------------------------------------------------------------------------------
// Purpose: called when a client spawns into a server (i.e as they begin to play)
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::ClientActive(edict_t *pEntity)
{
    int user_id = engine->GetPlayerUserId(pEntity);
    const char* networkId = engine->GetPlayerNetworkIDString(pEntity);

    KeyValues* event = new KeyValues("_client_active");
    event->SetInt("userid", user_id);
    if (networkId != NULL)
        event->SetString("networkid", networkId);
    LogEvent(event);
    event->deleteThis();
}

//---------------------------------------------------------------------------------
// Purpose: called when a client leaves a server (or is timed out)
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::ClientDisconnect(edict_t *pEntity)
{
    int user_id = engine->GetPlayerUserId(pEntity);
    const char* networkId = engine->GetPlayerNetworkIDString(pEntity);

    KeyValues* event = new KeyValues("_client_disconnect");
    event->SetInt("userid", user_id);
    if (networkId != NULL)
        event->SetString("networkid", networkId);
    LogEvent(event);
    event->deleteThis();
}

//---------------------------------------------------------------------------------
// Purpose: called on 
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::ClientPutInServer(edict_t *pEntity, char const *playername)
{
    int user_id = engine->GetPlayerUserId(pEntity);
    const char* networkId = engine->GetPlayerNetworkIDString(pEntity);

    KeyValues* event = new KeyValues("_client_put_in_server");
    event->SetString("player_name", playername);
    event->SetInt("userid", user_id);
    if (networkId != NULL)
        event->SetString("networkid", networkId);
    LogEvent(event);
    event->deleteThis();
}

//---------------------------------------------------------------------------------
// Purpose: called on level start
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::SetCommandClient(int index)
{
    m_iClientCommandIndex = index;
}

//---------------------------------------------------------------------------------
// Purpose: called on level start
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::ClientSettingsChanged(edict_t *pEdict)
{
}

//---------------------------------------------------------------------------------
// Purpose: called when a client joins a server
//---------------------------------------------------------------------------------
PLUGIN_RESULT CEventLoggerPlugin::ClientConnect( bool *bAllowConnect, edict_t *pEntity, const char *pszName, const char *pszAddress, char *reject, int maxrejectlen )
{
    int user_id = engine->GetPlayerUserId(pEntity);
    const char* networkId = engine->GetPlayerNetworkIDString(pEntity);

    KeyValues* event = new KeyValues("_client_connect");
    event->SetString("player_name", pszName);
    event->SetInt("userid", user_id);
    event->SetString("address", pszAddress);
    if (networkId != NULL)
        event->SetString("networkid", networkId);
    LogEvent(event);
    event->deleteThis();

    return PLUGIN_CONTINUE;
}

//---------------------------------------------------------------------------------
// Purpose: called when a client types in a command (only a subset of commands however, not CON_COMMAND's)
//---------------------------------------------------------------------------------
PLUGIN_RESULT CEventLoggerPlugin::ClientCommand( edict_t *pEntity, const CCommand &args )
{
    return PLUGIN_CONTINUE;
}

//---------------------------------------------------------------------------------
// Purpose: called when a client is authenticated
//---------------------------------------------------------------------------------
PLUGIN_RESULT CEventLoggerPlugin::NetworkIDValidated( const char *pszUserName, const char *pszNetworkID )
{
    KeyValues* event = new KeyValues("_network_id_validated");
    event->SetString("player_name", pszUserName);
    event->SetString("networkid", pszNetworkID);
    LogEvent(event);
    event->deleteThis();

    return PLUGIN_CONTINUE;
}

//---------------------------------------------------------------------------------
// Purpose: called when a cvar value query is finished
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::OnQueryCvarValueFinished( QueryCvarCookie_t iCookie, edict_t *pPlayerEntity, EQueryCvarValueStatus eStatus, const char *pCvarName, const char *pCvarValue )
{
}

//---------------------------------------------------------------------------------
// Purpose: called when an event is fired
//---------------------------------------------------------------------------------
void CEventLoggerPlugin::FireGameEvent(KeyValues * event)
{
    LogEvent(event);
}

void CEventLoggerPlugin::LogEvent(KeyValues* event)
{
    const char * name = event->GetName();

    if (m_db == NULL || PQstatus(m_db) != CONNECTION_OK)
        return;

    if (PQresultStatus(PQexec(m_db, "BEGIN TRANSACTION")) != PGRES_COMMAND_OK)
    {
        Warning("\"BEGIN TRANSACTION\" for event data failed: %s", PQerrorMessage(m_db));
        return;
    }

    bool dbFailure = false;
    PGresult* res;
    {
        const Oid paramTypes[] = { 23, 25 };
        const char* const values[] = { m_gameSessionId, name };
        const int lengths[] = { strlen(m_gameSessionId), strlen(name) };
        const int paramFormats[] = { 0, 0 };
        res = PQexecParams(m_db, "INSERT INTO Event (GameSessionId, Name) VALUES ($1, $2) RETURNING Id", 2, paramTypes, values, lengths, paramFormats, 0);
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            Warning("\"INSERT INTO Event\" failed\n");
            PQclear(res);
            if (PQresultStatus(PQexec(m_db, "ROLLBACK TRANSACTION")) != PGRES_COMMAND_OK)
                Warning("\"ROLLBACK TRANSACTION\" for failed event failed: %s", PQerrorMessage(m_db));
            return;
        }
    }

    char* eventId = strdup(PQgetvalue(res, 0, 0));
    PQclear(res);

    for (KeyValues *pKey = event->GetFirstSubKey(); pKey; pKey = pKey->GetNextKey())
    {
        const char* keyName = pKey->GetName();

        switch (pKey->GetDataType())
        {
        case KeyValues::TYPE_STRING:
            {
                const char* keyValue = pKey->GetString();

                const Oid paramTypes[] = { 23, 25, 25 };
                const char* const values[] = { eventId, keyName, keyValue };
                const int lengths[] = { strlen(eventId), strlen(keyName), strlen(keyValue) };
                const int paramFormats[] = { 0, 0, 0 };
                res = PQexecParams(m_db, "INSERT INTO EventData (EventId, Key, ValueString) VALUES ($1, $2, $3)", 3, paramTypes, values, lengths, paramFormats, 0);
                ExecStatusType resStatus = PQresultStatus(res);
                PQclear(res);
                if (resStatus != PGRES_COMMAND_OK)
                {
                    dbFailure = true;
                    Warning("\"INSERT INTO EventData\" for string data failed: %s\n", PQerrorMessage(m_db));
                }
            }
            break;
        case KeyValues::TYPE_INT:
            {
                int keyValue = pKey->GetInt();
                char keyValueStr[255];
                Q_snprintf(keyValueStr, 255, "%i", keyValue);

                const Oid paramTypes[] = { 23, 25, 23 };
                const char* const values[] = { eventId, keyName, keyValueStr };
                const int lengths[] = { strlen(eventId), strlen(keyName), strlen(keyValueStr) };
                const int paramFormats[] = { 0, 0, 0 };
                res = PQexecParams(m_db, "INSERT INTO EventData (EventId, Key, ValueInt) VALUES ($1, $2, $3)", 3, paramTypes, values, lengths, paramFormats, 0);
                ExecStatusType resStatus = PQresultStatus(res);
                PQclear(res);
                if (resStatus != PGRES_COMMAND_OK)
                {
                    dbFailure = true;
                    Warning("\"INSERT INTO EventData\" for int data failed: %s\n", PQerrorMessage(m_db));
                }
            }
            break;
        case KeyValues::TYPE_FLOAT:
            {
                float keyValue = pKey->GetFloat();
                char keyValueStr[255];
                Q_snprintf(keyValueStr, 255, "%f", keyValue);

                const Oid paramTypes[] = { 23, 25, 700 };
                const char* const values[] = { eventId, keyName, keyValueStr };
                const int lengths[] = { strlen(eventId), strlen(keyName), strlen(keyValueStr) };
                const int paramFormats[] = { 0, 0, 0 };
                res = PQexecParams(m_db, "INSERT INTO EventData (EventId, Key, ValueFloat) VALUES ($1, $2, $3)", 3, paramTypes, values, lengths, paramFormats, 0);
                ExecStatusType resStatus = PQresultStatus(res);
                PQclear(res);
                if (resStatus != PGRES_COMMAND_OK)
                {
                    dbFailure = true;
                    Warning("\"INSERT INTO EventData\" for float data failed: %s\n", PQerrorMessage(m_db));
                }
            }
            break;
        default:
            Warning("Event %s has key %s with data type <#%d> that could not be logged\n", name, pKey->GetName(), pKey->GetDataType());
            break;
        }
    }

    free(eventId);

    if (!dbFailure)
    {
        if (PQresultStatus(PQexec(m_db, "COMMIT TRANSACTION")) != PGRES_COMMAND_OK)
            Warning("\"COMMIT TRANSACTION\" for event failed: %s", PQerrorMessage(m_db));
    }
    else
    {
        if (PQresultStatus(PQexec(m_db, "ROLLBACK TRANSACTION")) != PGRES_COMMAND_OK)
            Warning("\"ROLLBACK TRANSACTION\" for failed event failed: %s", PQerrorMessage(m_db));
    }
}
