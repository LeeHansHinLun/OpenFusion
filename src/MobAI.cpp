#include "MobAI.hpp"
#include "Player.hpp"
#include "Racing.hpp"
#include "Transport.hpp"
#include "Nanos.hpp"
#include "Combat.hpp"
#include "Abilities.hpp"
#include "Rand.hpp"
#include "Items.hpp"
#include "Missions.hpp"

#include <cmath>
#include <limits.h>

using namespace MobAI;

bool MobAI::simulateMobs = settings::SIMULATEMOBS;

void Mob::step(time_t currTime) {
    if (playersInView < 0)
        std::cout << "[WARN] Weird playerview value " << playersInView << std::endl;

    // skip movement and combat if disabled or not in view
    if ((!MobAI::simulateMobs || playersInView == 0) && state != AIState::DEAD
        && state != AIState::RETREAT)
        return;

    // call superclass step
    CombatNPC::step(currTime);
}

int Mob::takeDamage(EntityRef src, int amt) {

    // cannot kill mobs multiple times; cannot harm retreating mobs
    if (state != AIState::ROAMING && state != AIState::COMBAT) {
        return 0; // no damage
    }

    if (skillStyle >= 0)
        return 0; // don't hurt a mob casting corruption

    if (state == AIState::ROAMING) {
        assert(target == nullptr && src.kind == EntityKind::PLAYER); // TODO: players only for now
        transition(AIState::COMBAT, src);

        if (groupLeader != 0)
            MobAI::followToCombat(this);
    }

    // wake up sleeping monster
    if (cbf & CSB_BIT_MEZ) {
        cbf &= ~CSB_BIT_MEZ;

        INITSTRUCT(sP_FE2CL_CHAR_TIME_BUFF_TIME_OUT, pkt1);
        pkt1.eCT = 2;
        pkt1.iID = id;
        pkt1.iConditionBitFlag = cbf;
        NPCManager::sendToViewable(this, &pkt1, P_FE2CL_CHAR_TIME_BUFF_TIME_OUT, sizeof(sP_FE2CL_CHAR_TIME_BUFF_TIME_OUT));
    }

    // call superclass takeDamage
    return CombatNPC::takeDamage(src, amt);
}

/*
 * Dynamic lerp; distinct from Transport::lerp(). This one doesn't care about height and
 * only returns the first step, since the rest will need to be recalculated anyway if chasing player.
 */
static std::pair<int,int> lerp(int x1, int y1, int x2, int y2, int speed) {
    std::pair<int,int> ret = {x1, y1};

    if (speed == 0)
        return ret;

    int distance = hypot(x1 - x2, y1 - y2);

    if (distance > speed) {

        int lerps = distance / speed;

        // interpolate only the first point
        float frac = 1.0f / lerps;

        ret.first = (x1 + (x2 - x1) * frac);
        ret.second = (y1 + (y2 - y1) * frac);
    } else {
        ret.first = x2;
        ret.second = y2;
    }

    return ret;
}

void MobAI::clearDebuff(Mob *mob) {
    mob->skillStyle = -1;
    mob->cbf = 0;
    mob->unbuffTimes.clear();

    INITSTRUCT(sP_FE2CL_CHAR_TIME_BUFF_TIME_OUT, pkt1);
    pkt1.eCT = 2;
    pkt1.iID = mob->id;
    pkt1.iConditionBitFlag = mob->cbf;
    NPCManager::sendToViewable(mob, &pkt1, P_FE2CL_CHAR_TIME_BUFF_TIME_OUT, sizeof(sP_FE2CL_CHAR_TIME_BUFF_TIME_OUT));
}

void MobAI::followToCombat(Mob *mob) {
    if (NPCManager::NPCs.find(mob->groupLeader) != NPCManager::NPCs.end() && NPCManager::NPCs[mob->groupLeader]->kind == EntityKind::MOB) {
        Mob* leadMob = (Mob*)NPCManager::NPCs[mob->groupLeader];
        for (int i = 0; i < 4; i++) {
            if (leadMob->groupMember[i] == 0)
                break;

            if (NPCManager::NPCs.find(leadMob->groupMember[i]) == NPCManager::NPCs.end() || NPCManager::NPCs[leadMob->groupMember[i]]->kind != EntityKind::MOB) {
                std::cout << "[WARN] roamingStep: leader can't find a group member!" << std::endl;
                continue;
            }
            Mob* followerMob = (Mob*)NPCManager::NPCs[leadMob->groupMember[i]];

            if (followerMob->state != AIState::ROAMING) // only roaming mobs should transition to combat
                continue;

            followerMob->transition(AIState::COMBAT, mob->target);
        }

        if (leadMob->state != AIState::ROAMING)
            return;

        leadMob->transition(AIState::COMBAT, mob->target);
    }
}

void MobAI::groupRetreat(Mob *mob) {
    if (NPCManager::NPCs.find(mob->groupLeader) == NPCManager::NPCs.end() || NPCManager::NPCs[mob->groupLeader]->kind != EntityKind::MOB)
        return;

    Mob* leadMob = (Mob*)NPCManager::NPCs[mob->groupLeader];
    for (int i = 0; i < 4; i++) {
        if (leadMob->groupMember[i] == 0)
            break;

        if (NPCManager::NPCs.find(leadMob->groupMember[i]) == NPCManager::NPCs.end() || NPCManager::NPCs[leadMob->groupMember[i]]->kind != EntityKind::MOB) {
            std::cout << "[WARN] roamingStep: leader can't find a group member!" << std::endl;
            continue;
        }
        Mob* followerMob = (Mob*)NPCManager::NPCs[leadMob->groupMember[i]];

        if (followerMob->state != AIState::COMBAT)
            continue;

        followerMob->target = nullptr;
        followerMob->state = AIState::RETREAT;
        clearDebuff(followerMob);
    }

    if (leadMob->state != AIState::COMBAT)
        return;

    leadMob->target = nullptr;
    leadMob->state = AIState::RETREAT;
    clearDebuff(leadMob);
}

/*
 * Aggro on nearby players.
 * Even if they're in range, we can't assume they're all in the same one chunk
 * as the mob, since it might be near a chunk boundary.
 */
bool MobAI::aggroCheck(Mob *mob, time_t currTime) {
    CNSocket *closest = nullptr;
    int closestDistance = INT_MAX;

    for (auto it = mob->viewableChunks.begin(); it != mob->viewableChunks.end(); it++) {
        Chunk* chunk = *it;
        for (const EntityRef& ref : chunk->entities) {
            // TODO: support targetting other CombatNPCs
            if (ref.kind != EntityKind::PLAYER)
                continue;

            CNSocket *s = ref.sock;
            Player *plr = PlayerManager::getPlayer(s);

            if (plr->HP <= 0 || plr->onMonkey)
                continue;

            int mobRange = mob->sightRange;

            if (plr->iConditionBitFlag & CSB_BIT_UP_STEALTH
            || Racing::EPRaces.find(s) != Racing::EPRaces.end())
                mobRange /= 3;

            // 0.33x - 1.66x the range
            int levelDifference = plr->level - mob->level;
            if (levelDifference > -10)
                mobRange = levelDifference < 10 ? mobRange - (levelDifference * mobRange / 15) : mobRange / 3;

            if (mob->state != AIState::ROAMING && plr->inCombat) // freshly out of aggro mobs
                mobRange = mob->sightRange * 2; // should not be impacted by the above

            if (plr->iSpecialState & (CN_SPECIAL_STATE_FLAG__INVISIBLE|CN_SPECIAL_STATE_FLAG__INVULNERABLE))
                mobRange = -1;

            // height is relevant for aggro distance because of platforming
            int xyDistance = hypot(mob->x - plr->x, mob->y - plr->y);
            int distance = hypot(xyDistance, (mob->z - plr->z) * 2); // difference in Z counts twice

            if (distance > mobRange || distance > closestDistance)
                continue;

            // found a player
            closest = s;
            closestDistance = distance;
        }
    }

    if (closest != nullptr) {
        // found closest player. engage.
        mob->transition(AIState::COMBAT, closest);

        if (mob->groupLeader != 0)
            followToCombat(mob);

        return true;
    }

    return false;
}

static void dealCorruption(Mob *mob, std::vector<int> targetData, int skillID, int style) {
    Player *plr = PlayerManager::getPlayer(mob->target);

    size_t resplen = sizeof(sP_FE2CL_NPC_SKILL_CORRUPTION_HIT) + targetData[0] * sizeof(sCAttackResult);

    // validate response packet
    if (!validOutVarPacket(sizeof(sP_FE2CL_NPC_SKILL_CORRUPTION_HIT), targetData[0], sizeof(sCAttackResult))) {
        std::cout << "[WARN] bad sP_FE2CL_NPC_SKILL_CORRUPTION_HIT packet size" << std::endl;
        return;
    }

    uint8_t respbuf[CN_PACKET_BUFFER_SIZE];
    memset(respbuf, 0, resplen);

    sP_FE2CL_NPC_SKILL_CORRUPTION_HIT *resp = (sP_FE2CL_NPC_SKILL_CORRUPTION_HIT*)respbuf;
    sCAttackResult *respdata = (sCAttackResult*)(respbuf+sizeof(sP_FE2CL_NPC_SKILL_CORRUPTION_HIT));

    resp->iNPC_ID = mob->id;
    resp->iSkillID = skillID;
    resp->iStyle = style;
    resp->iValue1 = plr->x;
    resp->iValue2 = plr->y;
    resp->iValue3 = plr->z;
    resp->iTargetCnt = targetData[0];

    for (int i = 0; i < targetData[0]; i++) {
        CNSocket *sock = nullptr;
        Player *plr = nullptr;

        for (auto& pair : PlayerManager::players) {
            if (pair.second->iID == targetData[i+1]) {
                sock = pair.first;
                plr = pair.second;
                break;
            }
        }

        // player not found
        if (plr == nullptr) {
            std::cout << "[WARN] dealCorruption: player ID not found" << std::endl;
            return;
        }

        respdata[i].eCT = 1;
        respdata[i].iID = plr->iID;
        respdata[i].bProtected = 0;

        respdata[i].iActiveNanoSlotNum = -1;
        for (int n = 0; n < 3; n++)
            if (plr->activeNano == plr->equippedNanos[n])
                respdata[i].iActiveNanoSlotNum = n;
        respdata[i].iNanoID = plr->activeNano;

        int style2 = Nanos::nanoStyle(plr->activeNano);
        if (style2 == -1) { // no nano
            respdata[i].iHitFlag = HF_BIT_STYLE_TIE;
            respdata[i].iDamage = Abilities::SkillTable[skillID].powerIntensity[0] * PC_MAXHEALTH((int)mob->data["m_iNpcLevel"]) / 1500;
        } else if (style == style2) {
            respdata[i].iHitFlag = HF_BIT_STYLE_TIE;
            respdata[i].iDamage = 0;
            respdata[i].iNanoStamina = plr->Nanos[plr->activeNano].iStamina;
        } else if (style - style2 == 1 || style2 - style == 2) {
            respdata[i].iHitFlag = HF_BIT_STYLE_WIN;
            respdata[i].iDamage = 0;
            respdata[i].iNanoStamina = plr->Nanos[plr->activeNano].iStamina += 45;
            if (plr->Nanos[plr->activeNano].iStamina > 150)
                respdata[i].iNanoStamina = plr->Nanos[plr->activeNano].iStamina = 150;
            // fire damage power disguised as a corruption attack back at the enemy
            // TODO ABILITIES
            /*std::vector<int> targetData2 = {1, mob->id, 0, 0, 0};
            for (auto& pwr : Abilities::Powers)
                if (pwr.skillType == EST_DAMAGE)
                    pwr.handle(sock, targetData2, plr->activeNano, skillID, 0, 200);*/
        } else {
            respdata[i].iHitFlag = HF_BIT_STYLE_LOSE;
            respdata[i].iDamage = Abilities::SkillTable[skillID].powerIntensity[0] * PC_MAXHEALTH((int)mob->data["m_iNpcLevel"]) / 1500;
            respdata[i].iNanoStamina = plr->Nanos[plr->activeNano].iStamina -= 90;
            if (plr->Nanos[plr->activeNano].iStamina < 0) {
                respdata[i].bNanoDeactive = 1;
                respdata[i].iNanoStamina = plr->Nanos[plr->activeNano].iStamina = 0;
            }
        }

        if (!(plr->iSpecialState & CN_SPECIAL_STATE_FLAG__INVULNERABLE))
            plr->HP -= respdata[i].iDamage;

        respdata[i].iHP = plr->HP;
        respdata[i].iConditionBitFlag = plr->iConditionBitFlag;

        if (plr->HP <= 0) {
            if (!MobAI::aggroCheck(mob, getTime()))
                mob->transition(AIState::RETREAT, mob->target);
        }
    }

    NPCManager::sendToViewable(mob, (void*)&respbuf, P_FE2CL_NPC_SKILL_CORRUPTION_HIT, resplen);
}

static void useAbilities(Mob *mob, time_t currTime) {
    /*
     * targetData approach
     * first integer is the count
     * second to fifth integers are IDs, these can be either player iID or mob's iID
     * whether the skill targets players or mobs is determined by the skill packet being fired
     */
    Player *plr = PlayerManager::getPlayer(mob->target);

    if (mob->skillStyle >= 0) { // corruption hit
        int skillID = (int)mob->data["m_iCorruptionType"];
        std::vector<int> targetData = {1, plr->iID, 0, 0, 0};
        int temp = mob->skillStyle;
        mob->skillStyle = -3; // corruption cooldown
        mob->nextAttack = currTime + 1000;
        dealCorruption(mob, targetData, skillID, temp);
        return;
    }

    if (mob->skillStyle == -2) { // eruption hit
        int skillID = (int)mob->data["m_iMegaType"];
        std::vector<int> targetData = {0, 0, 0, 0, 0};

        // find the players within range of eruption
        for (auto it = mob->viewableChunks.begin(); it != mob->viewableChunks.end(); it++) {
            Chunk* chunk = *it;
            for (const EntityRef& ref : chunk->entities) {
                // TODO: see aggroCheck()
                if (ref.kind != EntityKind::PLAYER)
                    continue;

                CNSocket *s= ref.sock;
                Player *plr = PlayerManager::getPlayer(s);

                if (plr->HP <= 0)
                    continue;

                int distance = hypot(mob->hitX - plr->x, mob->hitY - plr->y);
                if (distance < Abilities::SkillTable[skillID].effectArea) {
                    targetData[0] += 1;
                    targetData[targetData[0]] = plr->iID;
                    if (targetData[0] > 3) // make sure not to have more than 4
                        break;
                }
            }
        }

        // TODO ABILITIES
        /*for (auto& pwr : Abilities::Powers)
            if (pwr.skillType == Abilities::SkillTable[skillID].skillType)
                pwr.handle(mob->id, targetData, skillID, Abilities::SkillTable[skillID].durationTime[0], Abilities::SkillTable[skillID].powerIntensity[0]);*/
        mob->skillStyle = -3; // eruption cooldown
        mob->nextAttack = currTime + 1000;
        return;
    }

    if (mob->skillStyle == -3) { // cooldown expires
        mob->skillStyle = -1;
        return;
    }

    int random = Rand::rand(2000) * 1000;
    int prob1 = (int)mob->data["m_iActiveSkill1Prob"]; // active skill probability
    int prob2 = (int)mob->data["m_iCorruptionTypeProb"]; // corruption probability
    int prob3 = (int)mob->data["m_iMegaTypeProb"]; // eruption probability

    if (random < prob1) { // active skill hit
        int skillID = (int)mob->data["m_iActiveSkill1"];
        // TODO ABILITIES
        //std::vector<int> targetData = {1, plr->iID, 0, 0, 0};
        //for (auto& pwr : Abilities::Powers)
        //    if (pwr.skillType == Abilities::SkillTable[skillID].skillType) {
        //        if (pwr.bitFlag != 0 && (plr->iConditionBitFlag & pwr.bitFlag))
        //            return; // prevent debuffing a player twice
        //        pwr.handle(mob->id, targetData, skillID, Abilities::SkillTable[skillID].durationTime[0], Abilities::SkillTable[skillID].powerIntensity[0]);
        //    }
        mob->nextAttack = currTime + (int)mob->data["m_iDelayTime"] * 100;
        return;
    }

    if (random < prob1 + prob2) { // corruption windup
        int skillID = (int)mob->data["m_iCorruptionType"];
        INITSTRUCT(sP_FE2CL_NPC_SKILL_CORRUPTION_READY, pkt);
        pkt.iNPC_ID = mob->id;
        pkt.iSkillID = skillID;
        pkt.iValue1 = plr->x;
        pkt.iValue2 = plr->y;
        pkt.iValue3 = plr->z;
        mob->skillStyle = Nanos::nanoStyle(plr->activeNano) - 1;
        if (mob->skillStyle == -1)
            mob->skillStyle = 2;
        if (mob->skillStyle == -2)
            mob->skillStyle = Rand::rand(3);
        pkt.iStyle = mob->skillStyle;
        NPCManager::sendToViewable(mob, &pkt, P_FE2CL_NPC_SKILL_CORRUPTION_READY, sizeof(sP_FE2CL_NPC_SKILL_CORRUPTION_READY));
        mob->nextAttack = currTime + 1800;
        return;
    }

    if (random < prob1 + prob2 + prob3) { // eruption windup
        int skillID = (int)mob->data["m_iMegaType"];
        INITSTRUCT(sP_FE2CL_NPC_SKILL_READY, pkt);
        pkt.iNPC_ID = mob->id;
        pkt.iSkillID = skillID;
        pkt.iValue1 = mob->hitX = plr->x;
        pkt.iValue2 = mob->hitY = plr->y;
        pkt.iValue3 = mob->hitZ = plr->z;
        NPCManager::sendToViewable(mob, &pkt, P_FE2CL_NPC_SKILL_READY, sizeof(sP_FE2CL_NPC_SKILL_READY));
        mob->nextAttack = currTime + 1800;
        mob->skillStyle = -2;
        return;
    }

    return;
}

static void drainMobHP(Mob *mob, int amount) {
    size_t resplen = sizeof(sP_FE2CL_CHAR_TIME_BUFF_TIME_TICK) + sizeof(sSkillResult_Damage);
    assert(resplen < CN_PACKET_BUFFER_SIZE - 8);
    uint8_t respbuf[CN_PACKET_BUFFER_SIZE];

    memset(respbuf, 0, resplen);

    sP_FE2CL_CHAR_TIME_BUFF_TIME_TICK *pkt = (sP_FE2CL_CHAR_TIME_BUFF_TIME_TICK*)respbuf;
    sSkillResult_Damage *drain = (sSkillResult_Damage*)(respbuf + sizeof(sP_FE2CL_CHAR_TIME_BUFF_TIME_TICK));

    pkt->iID = mob->id;
    pkt->eCT = 4; // mob
    pkt->iTB_ID = ECSB_BOUNDINGBALL;

    drain->eCT = 4;
    drain->iID = mob->id;
    drain->iDamage = amount;
    drain->iHP = mob->hp -= amount;

    NPCManager::sendToViewable(mob, (void*)&respbuf, P_FE2CL_CHAR_TIME_BUFF_TIME_TICK, resplen);

    if (mob->hp <= 0)
        mob->transition(AIState::DEAD, mob->target);
}

void MobAI::incNextMovement(Mob* mob, time_t currTime) {
    if (currTime == 0)
        currTime = getTime();

    int delay = (int)mob->data["m_iDelayTime"] * 1000;
    mob->nextMovement = currTime + delay / 2 + Rand::rand(delay / 2);
}

void MobAI::deadStep(CombatNPC* npc, time_t currTime) {
    Mob* self = (Mob*)npc;

    // despawn the mob after a short delay
    if (self->killedTime != 0 && !self->despawned && currTime - self->killedTime > 2000) {
        self->despawned = true;

        INITSTRUCT(sP_FE2CL_NPC_EXIT, pkt);

        pkt.iNPC_ID = self->id;

        NPCManager::sendToViewable(self, &pkt, P_FE2CL_NPC_EXIT, sizeof(sP_FE2CL_NPC_EXIT));

        // if it was summoned, mark it for removal
        if (self->summoned) {
            std::cout << "[INFO] Queueing killed summoned mob for removal" << std::endl;
            NPCManager::queueNPCRemoval(self->id);
            return;
        }

        // pre-set spawn coordinates if not marked for removal
        self->x = self->spawnX;
        self->y = self->spawnY;
        self->z = self->spawnZ;
    }

    // to guide their groupmates, group leaders still need to move despite being dead
    if (self->groupLeader == self->id)
        roamingStep(self, currTime);

    if (self->killedTime != 0 && currTime - self->killedTime < self->regenTime * 100)
        return;

    std::cout << "respawning mob " << self->id << " with HP = " << self->maxHealth << std::endl;

    self->transition(AIState::ROAMING, self->id);

    // if mob is a group leader/follower, spawn where the group is.
    if (self->groupLeader != 0) {
        if (NPCManager::NPCs.find(self->groupLeader) != NPCManager::NPCs.end() && NPCManager::NPCs[self->groupLeader]->kind == EntityKind::MOB) {
            Mob* leaderMob = (Mob*)NPCManager::NPCs[self->groupLeader];
            self->x = leaderMob->x + self->offsetX;
            self->y = leaderMob->y + self->offsetY;
            self->z = leaderMob->z;
        } else {
            std::cout << "[WARN] deadStep: mob cannot find it's leader!" << std::endl;
        }
    }

    INITSTRUCT(sP_FE2CL_NPC_NEW, pkt);

    pkt.NPCAppearanceData = self->getAppearanceData();

    // notify all nearby players
    NPCManager::sendToViewable(self, &pkt, P_FE2CL_NPC_NEW, sizeof(sP_FE2CL_NPC_NEW));
}

void MobAI::combatStep(CombatNPC* npc, time_t currTime) {
    Mob* self = (Mob*)npc;
    assert(self->target != nullptr);

    // lose aggro if the player lost connection
    if (PlayerManager::players.find(self->target) == PlayerManager::players.end()) {
        if (!MobAI::aggroCheck(self, getTime()))
            self->transition(AIState::RETREAT, self->target);
        return;
    }

    Player *plr = PlayerManager::getPlayer(self->target);

    // lose aggro if the player became invulnerable or died
    if (plr->HP <= 0
     || (plr->iSpecialState & CN_SPECIAL_STATE_FLAG__INVULNERABLE)) {
        if (!MobAI::aggroCheck(self, getTime()))
            self->transition(AIState::RETREAT, self->target);
        return;
    }

    // drain
    if (self->skillStyle < 0 && (self->lastDrainTime == 0 || currTime - self->lastDrainTime >= 1000)
        && self->cbf & CSB_BIT_BOUNDINGBALL) {
        drainMobHP(self, self->maxHealth / 20); // lose 5% every second
        self->lastDrainTime = currTime;
    }

    // if drain killed the mob, return early
    if (self->hp <= 0)
        return;

    // unbuffing
    std::unordered_map<int32_t, time_t>::iterator it = self->unbuffTimes.begin();
    while (it != self->unbuffTimes.end()) {

        if (currTime >= it->second) {
            self->cbf &= ~it->first;

            INITSTRUCT(sP_FE2CL_CHAR_TIME_BUFF_TIME_OUT, pkt1);
            pkt1.eCT = 2;
            pkt1.iID = self->id;
            pkt1.iConditionBitFlag = self->cbf;
            NPCManager::sendToViewable(self, &pkt1, P_FE2CL_CHAR_TIME_BUFF_TIME_OUT, sizeof(sP_FE2CL_CHAR_TIME_BUFF_TIME_OUT));

            it = self->unbuffTimes.erase(it);
        } else {
            it++;
        }
    }

    // skip attack if stunned or asleep
    if (self->cbf & (CSB_BIT_STUN|CSB_BIT_MEZ)) {
        self->skillStyle = -1; // in this case we also reset the any outlying abilities the mob might be winding up.
        return;
    }

    int distance = hypot(plr->x - self->x, plr->y - self->y);
    int mobRange = (int)self->data["m_iAtkRange"] + (int)self->data["m_iRadius"];

    if (currTime >= self->nextAttack) {
        if (self->skillStyle != -1 || distance <= mobRange || Rand::rand(20) == 0) // while not in attack range, 1 / 20 chance.
            useAbilities(self, currTime);
        if (self->target == nullptr)
            return;
    }

    int distanceToTravel = INT_MAX;
    // movement logic: move when out of range but don't move while casting a skill
    if (distance > mobRange && self->skillStyle == -1) {
        if (self->nextMovement != 0 && currTime < self->nextMovement)
            return;
        self->nextMovement = currTime + 400;
        if (currTime >= self->nextAttack)
            self->nextAttack = 0;

        // halve movement speed if snared
        if (self->cbf & CSB_BIT_DN_MOVE_SPEED)
            self->speed /= 2;

        int targetX = plr->x;
        int targetY = plr->y;
        if (self->groupLeader != 0) {
            targetX += self->offsetX*distance/(self->idleRange + 1);
            targetY += self->offsetY*distance/(self->idleRange + 1);
        }

        distanceToTravel = std::min(distance-mobRange+1, self->speed*2/5);
        auto targ = lerp(self->x, self->y, targetX, targetY, distanceToTravel);
        if (distanceToTravel < self->speed*2/5 && currTime >= self->nextAttack)
            self->nextAttack = 0;

        NPCManager::updateNPCPosition(self->id, targ.first, targ.second, self->z, self->instanceID, self->angle);

        INITSTRUCT(sP_FE2CL_NPC_MOVE, pkt);

        pkt.iNPC_ID = self->id;
        pkt.iSpeed = self->speed;
        pkt.iToX = self->x = targ.first;
        pkt.iToY = self->y = targ.second;
        pkt.iToZ = plr->z;
        pkt.iMoveStyle = 1;

        // notify all nearby players
        NPCManager::sendToViewable(self, &pkt, P_FE2CL_NPC_MOVE, sizeof(sP_FE2CL_NPC_MOVE));
    }

    /* attack logic
     * 2/5 represents 400 ms which is the time interval mobs use per movement logic step
     * if the mob is one move interval away, we should just start attacking anyways.
     */
    if (distance <= mobRange || distanceToTravel < self->speed*2/5) {
        if (self->nextAttack == 0 || currTime >= self->nextAttack) {
            self->nextAttack = currTime + (int)self->data["m_iDelayTime"] * 100;
            Combat::npcAttackPc(self, currTime);
        }
    }

    // retreat if the player leaves combat range
    int xyDistance = hypot(plr->x - self->roamX, plr->y - self->roamY);
    distance = hypot(xyDistance, plr->z - self->roamZ);
    if (distance >= self->data["m_iCombatRange"]) {
        self->transition(AIState::RETREAT, self->target);
    }
}

void MobAI::roamingStep(CombatNPC* npc, time_t currTime) {
    Mob* self = (Mob*)npc;

    /*
     * We reuse nextAttack to avoid scanning for players all the time, but to still
     * do so more often than if we waited for nextMovement (which is way too slow).
     * In the case of group leaders, this step will be called by dead mobs, so disable attack.
     */
    if (self->state != AIState::DEAD && (self->nextAttack == 0 || currTime >= self->nextAttack)) {
        self->nextAttack = currTime + 500;
        if (aggroCheck(self, currTime))
            return;
    }

    // no random roaming if the mob already has a set path
    if (self->staticPath)
        return;

    if (self->groupLeader != 0 && self->groupLeader != self->id) // don't roam by yourself without group leader
        return;

    /*
     * mob->nextMovement is also updated whenever the path queue is traversed in
     * Transport::stepNPCPathing() (which ticks at a higher frequency than nextMovement),
     * so we don't have to check if there's already entries in the queue since we know there won't be.
     */
    if (self->nextMovement != 0 && currTime < self->nextMovement)
        return;
    incNextMovement(self, currTime);

    int xStart = self->spawnX - self->idleRange/2;
    int yStart = self->spawnY - self->idleRange/2;

    // some mobs don't move (and we mustn't divide/modulus by zero)
    if (self->idleRange == 0 || self->speed == 0)
        return;

    int farX, farY, distance;
    int minDistance = self->idleRange / 2;

    // pick a random destination
    farX = xStart + Rand::rand(self->idleRange);
    farY = yStart + Rand::rand(self->idleRange);

    distance = std::abs(std::max(farX - self->x, farY - self->y));
    if (distance == 0)
        distance += 1; // hack to avoid FPE

    // if it's too short a walk, go further in that direction
    farX = self->x + (farX - self->x) * minDistance / distance;
    farY = self->y + (farY - self->y) * minDistance / distance;

    // but don't got out of bounds
    farX = std::clamp(farX, xStart, xStart + self->idleRange);
    farY = std::clamp(farY, yStart, yStart + self->idleRange);

    // halve movement speed if snared
    if (self->cbf & CSB_BIT_DN_MOVE_SPEED)
        self->speed /= 2;

    std::queue<Vec3> queue;
    Vec3 from = { self->x, self->y, self->z };
    Vec3 to = { farX, farY, self->z };

    // add a route to the queue; to be processed in Transport::stepNPCPathing()
    Transport::lerp(&queue, from, to, self->speed);
    Transport::NPCQueues[self->id] = queue;

    if (self->groupLeader != 0 && self->groupLeader == self->id) {
        // make followers follow this npc.
        for (int i = 0; i < 4; i++) {
            if (self->groupMember[i] == 0)
                break;

            if (NPCManager::NPCs.find(self->groupMember[i]) == NPCManager::NPCs.end() || NPCManager::NPCs[self->groupMember[i]]->kind != EntityKind::MOB) {
                std::cout << "[WARN] roamingStep: leader can't find a group member!" << std::endl;
                continue;
            }

            std::queue<Vec3> queue2;
            Mob* followerMob = (Mob*)NPCManager::NPCs[self->groupMember[i]];
            from = { followerMob->x, followerMob->y, followerMob->z };
            to = { farX + followerMob->offsetX, farY + followerMob->offsetY, followerMob->z };
            Transport::lerp(&queue2, from, to, self->speed);
            Transport::NPCQueues[followerMob->id] = queue2;
        }
    }
}

void MobAI::retreatStep(CombatNPC* npc, time_t currTime) {
    Mob* self = (Mob*)npc;

    if (self->nextMovement != 0 && currTime < self->nextMovement)
        return;

    self->nextMovement = currTime + 400;

    // distance between spawn point and current location
    int distance = hypot(self->x - self->roamX, self->y - self->roamY);

    //if (distance > mob->data["m_iIdleRange"]) {
    if (distance > 10) {
        INITSTRUCT(sP_FE2CL_NPC_MOVE, pkt);

        auto targ = lerp(self->x, self->y, self->roamX, self->roamY, (int)self->speed*4/5);

        pkt.iNPC_ID = self->id;
        pkt.iSpeed = (int)self->speed * 2;
        pkt.iToX = self->x = targ.first;
        pkt.iToY = self->y = targ.second;
        pkt.iToZ = self->z = self->spawnZ;
        pkt.iMoveStyle = 1;

        // notify all nearby players
        NPCManager::sendToViewable(self, &pkt, P_FE2CL_NPC_MOVE, sizeof(sP_FE2CL_NPC_MOVE));
    }

    // if we got there
    //if (distance <= mob->data["m_iIdleRange"]) {
    if (distance <= 10) { // retreat back to the spawn point
        self->transition(AIState::ROAMING, self->id);
    }
}

void MobAI::onRoamStart(CombatNPC* npc, EntityRef src) {
    Mob* self = (Mob*)npc;

    self->hp = self->maxHealth;
    self->killedTime = 0;
    self->nextAttack = 0;
    self->cbf = 0;

    // cast a return home heal spell, this is the right way(tm)
    // TODO ABILITIES
    /*std::vector<int> targetData = { 1, 0, 0, 0, 0 };
    for (auto& pwr : Abilities::Powers)
        if (pwr.skillType == Abilities::SkillTable[110].skillType)
            pwr.handle(self->id, targetData, 110, Abilities::SkillTable[110].durationTime[0], Abilities::SkillTable[110].powerIntensity[0]);*/
    // clear outlying debuffs
    clearDebuff(self);
}

void MobAI::onCombatStart(CombatNPC* npc, EntityRef src) {
    Mob* self = (Mob*)npc;

    assert(src.kind == EntityKind::PLAYER);
    self->target = src.sock;
    self->nextMovement = getTime();
    self->nextAttack = 0;

    self->roamX = self->x;
    self->roamY = self->y;
    self->roamZ = self->z;

    int skillID = (int)self->data["m_iPassiveBuff"]; // cast passive
    // TODO ABILITIES
    /*std::vector<int> targetData = { 1, self->id, 0, 0, 0 };
    for (auto& pwr : Abilities::Powers)
        if (pwr.skillType == Abilities::SkillTable[skillID].skillType)
            pwr.handle(self->id, targetData, skillID, Abilities::SkillTable[skillID].durationTime[0], Abilities::SkillTable[skillID].powerIntensity[0]);*/
}

void MobAI::onRetreat(CombatNPC* npc, EntityRef src) {
    Mob* self = (Mob*)npc;

    self->target = nullptr;
    MobAI::clearDebuff(self);
    if (self->groupLeader != 0)
        MobAI::groupRetreat(self);
}

void MobAI::onDeath(CombatNPC* npc, EntityRef src) {
    Mob* self = (Mob*)npc;

    self->target = nullptr;
    self->cbf = 0;
    self->skillStyle = -1;
    self->unbuffTimes.clear();
    self->killedTime = getTime(); // XXX: maybe introduce a shard-global time for each step?

    // check for the edge case where hitting the mob did not aggro it
    if (src.kind == EntityKind::PLAYER && src.isValid()) {
        Player* plr = PlayerManager::getPlayer(src.sock);

        Items::DropRoll rolled;
        Items::DropRoll eventRolled;
        std::map<int, int> qitemRolls;

        Player* leader = PlayerManager::getPlayerFromID(plr->iIDGroup);
        assert(leader != nullptr); // should never happen

        Combat::genQItemRolls(leader, qitemRolls);

        if (plr->groupCnt == 1 && plr->iIDGroup == plr->iID) {
            Items::giveMobDrop(src.sock, self, rolled, eventRolled);
            Missions::mobKilled(src.sock, self->type, qitemRolls);
        }
        else {
            for (int i = 0; i < leader->groupCnt; i++) {
                CNSocket* sockTo = PlayerManager::getSockFromID(leader->groupIDs[i]);
                if (sockTo == nullptr)
                    continue;

                Player* otherPlr = PlayerManager::getPlayer(sockTo);

                // only contribute to group members' kills if they're close enough
                int dist = std::hypot(plr->x - otherPlr->x + 1, plr->y - otherPlr->y + 1);
                if (dist > 5000)
                    continue;

                Items::giveMobDrop(sockTo, self, rolled, eventRolled);
                Missions::mobKilled(sockTo, self->type, qitemRolls);
            }
        }
    }

    // delay the despawn animation
    self->despawned = false;

    auto it = Transport::NPCQueues.find(self->id);
    if (it == Transport::NPCQueues.end() || it->second.empty())
        return;

    // rewind or empty the movement queue
    if (self->staticPath) {
        /*
         * This is inelegant, but we wind forward in the path until we find the point that
         * corresponds with the Mob's spawn point.
         *
         * IMPORTANT: The check in TableData::loadPaths() must pass or else this will loop forever.
         */
        auto& queue = it->second;
        for (auto point = queue.front(); point.x != self->spawnX || point.y != self->spawnY; point = queue.front()) {
            queue.pop();
            queue.push(point);
        }
    }
    else {
        Transport::NPCQueues.erase(self->id);
    }
}
