/*
 * Copyright © 2018 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Daniel Schürmann (daniel.schuermann@campus.tu-berlin.de)
 *
 */

#ifndef ACO_IR_H
#define ACO_IR_H

#include <ostream>
#include <vector>
#include <set>

#include "nir/nir.h"
#include "common/ac_binary.h"
#include "common/amd_family.h"
#include "aco_opcodes.h"


struct radv_shader_variant_info;
struct radv_nir_compiler_options;

typedef enum {
   b = 0,
   s1 = 1,
   s2 = 2,
   s3 = 3,
   s4 = 4,
   s8 = 8,
   s16 = 16,
   v1 = s1 | (1 << 5),
   v2 = s2 | (1 << 5),
   v3 = s3 | (1 << 5),
   v4 = s4 | (1 << 5),
   v6 = 6  | (1 << 5),
} RegClass;

typedef struct {
   const char *name;
   unsigned num_inputs;
   unsigned num_outputs;
   RegClass output_type[2];
   bool kills_input[4];
   /* TODO: everything that depends on the instruction rather than the format */
   // like sideeffects on spr's
   unsigned opcode;
} opcode_info;

extern const opcode_info opcode_infos[static_cast<int>(aco_opcode::num_opcodes)];

#ifdef __cplusplus
#include <cstdint>
#include <vector>
#include <bitset>
#include <deque>
#include <memory>

namespace aco {

/**
 * Representation of the instruction's microcode encoding format
 * Note: Some Vector ALU Formats can be combined, such that:
 * - VOP2* | VOP3A represents a VOP2 instruction in VOP3A encoding
 * - VOP2* | DPP represents a VOP2 instruction with data parallel primitive.
 * - VOP2* | SDWA represents a VOP2 instruction with sub-dword addressing.
 * 
 * (*) The same is applicable for VOP1 and VOPC instructions.
 */
enum class Format : std::uint16_t {
   /* Pseudo Instruction Format */
   PSEUDO = 0,
   /* Scalar ALU & Control Formats */
   SOP1 = 1,
   SOP2 = 2,
   SOPK = 3,
   SOPP = 4,
   SOPC = 5,
   /* Scalar Memory Format */
   SMEM = 6,
   /* Vector Parameter Interpolation Format */
   VINTRP = 7,
   /* LDS/GDS Format */
   DS = 8,
   /* Vector Memory Buffer Formats */
   MTBUF = 9,
   MUBUF = 10,
   /* Vector Memory Image Format */
   MIMG = 11,
   /* Export Format */
   EXP = 12,
   /* Flat Formats */
   FLAT = 13,
   GLOBAL = 14,
   SCRATCH = 15,

   PSEUDO_BRANCH = 16,

   /* Vector ALU Formats */
   VOP1 = 1 << 8,
   VOP2 = 1 << 9,
   VOPC = 1 << 10,
   VOP3B = 1 <<11,
   VOP3P = 1 << 12,
   VOP3A = 1 << 13,
   DPP = 1 << 14,
   SDWA = 1 << 15,
};

enum RegType {
   scc,
   sgpr,
   vgpr,
};

static inline RegType typeOf(RegClass rc)
{
   if (rc == RegClass::b) return RegType::scc;
   if (rc <= RegClass::s16) return RegType::sgpr;
   else return RegType::vgpr;
}

static inline unsigned sizeOf(RegClass rc)
{
   return (unsigned) rc & 0x1F;
}

static inline RegClass getRegClass(RegType type, unsigned size)
{
   return type == scc ? b : (RegClass) ((type == vgpr ? 1 << 5 : 0) | size);
}

/**
 * Temp Class
 * Each temporary virtual register has a
 * register class (i.e. size and type)
 * and SSA id.
 */
class Temp {
public:
   Temp() = default;
   constexpr Temp(uint32_t id, RegClass cls) noexcept
      : id_(id), reg_class(cls) {}

   uint32_t id() const noexcept
   {
      return id_;
   }

   unsigned size() const noexcept
   {
      return sizeOf(reg_class);
   }

   RegType type() const noexcept
   {
      return typeOf(reg_class);
   }

   RegClass regClass() const noexcept
   {
      return reg_class;
   }

   bool operator<(Temp other) const noexcept
   {
      return id() < other.id();
   }

   bool operator ==(Temp other) const noexcept
   {
      return id() == other.id();
   }

private:
   uint32_t id_;
   RegClass reg_class;
};

struct PhysReg
{
   unsigned reg;
   bool operator==(const PhysReg& rhs) const
   {
      return reg == rhs.reg;
   }
   bool operator<(const PhysReg& rhs) const
   {
      return reg < rhs.reg;
   }
};

static inline PhysReg fixed_vgpr(unsigned idx)
{
   return PhysReg{idx + 256};
}

static inline PhysReg fixed_sgpr(unsigned idx)
{
   return PhysReg{idx};
}

static constexpr PhysReg m0{124};
static constexpr PhysReg vcc{106};
static constexpr PhysReg exec{126};

/**
 * Operand Class
 * Initially, each Operand refers to either
 * a temporary virtual register
 * or to a constant value
 * Temporary registers get mapped to physical register during RA
 * Constant values are inlined into the instruction sequence.
 */
class Operand final
{
public:
   explicit Operand(Temp r) noexcept
   {
      data_.temp = r;
      control_[0] = 1; /* isTemp */
   };
   explicit Operand(uint32_t v) noexcept
   {
      data_.i = v;
      control_[2] = 1; /* isConst */
      if (v <= 64)
         setFixed(PhysReg{128 + v});
      else if (v >= 0xFFFFFFF0) /* [-16 .. -1] */
         setFixed(PhysReg{192 - v});
      else if (v == 0x3f000000) /* 0.5 */
         setFixed(PhysReg{240});
      else if (v == 0xbf000000) /* -0.5 */
         setFixed(PhysReg{241});
      else if (v == 0x3f800000) /* 1.0 */
         setFixed(PhysReg{242});
      else if (v == 0xbf800000) /* -1.0 */
         setFixed(PhysReg{243});
      else if (v == 0x40000000) /* 2.0 */
         setFixed(PhysReg{244});
      else if (v == 0xc0000000) /* -2.0 */
         setFixed(PhysReg{245});
      else if (v == 0x40800000) /* 4.0 */
         setFixed(PhysReg{246});
      else if (v == 0xc0800000) /* -4.0 */
         setFixed(PhysReg{247});
      else if (v == 0x3e22f983) /* 1/(2*PI) */
         setFixed(PhysReg{248});
      else /* Literal Constant */
         setFixed(PhysReg{255});
   };
   explicit Operand() noexcept
   {
      control_[4] = 1; /* undefined */
      setFixed(PhysReg{128});
   };
   explicit Operand(PhysReg reg, RegClass type) noexcept
   {
      data_.temp = Temp(0, type);
      setFixed(reg);
   }

   bool isTemp() const noexcept
   {
      return control_[0];
   }

   void setTemp(Temp t) {
      assert(!control_[2]);
      control_[0] = 1; /* isTemp */
      data_.temp = t;
   }

   Temp getTemp() const noexcept
   {
      return data_.temp;
   }

   uint32_t tempId() const noexcept
   {
      return data_.temp.id();
   }

   RegClass regClass() const noexcept
   {
      return data_.temp.regClass();
   }

   unsigned size() const noexcept
   {
      if (isConstant() || isUndefined())
         return 1;
      else
         return data_.temp.size();
   }

   bool isFixed() const noexcept
   {
      return control_[1];
   }

   PhysReg physReg() const noexcept
   {
      return reg_;
   }

   void setFixed(PhysReg reg) noexcept
   {
      control_[1] = 1;
      reg_ = reg;
   }

   bool isConstant() const noexcept
   {
      return control_[2];
   }

   bool isLiteral() const noexcept
   {
      return isConstant() && reg_.reg == 255;
   }

   bool isUndefined() const noexcept
   {
      return control_[4];
   }

   uint32_t constantValue() const noexcept
   {
      return data_.i;
   }

   void setKill(bool) noexcept
   {
      control_[3] = 1;
   }

   bool isKill() const noexcept
   {
      return control_[3];
   }

private:
   union {
      uint32_t i;
      float f;
      Temp temp;
   } data_;
   PhysReg reg_;
   std::bitset<8> control_;
};

/**
 * Definition Class
 * Definitions are the results of Instructions
 * and refer to temporary virtual registers
 * which are later mapped to physical registers
 */
class Definition final
{
public:
   Definition() = default;
   Definition(uint32_t index, RegClass type) noexcept
      : temp(index, type) {}
   Definition(Temp tmp) noexcept
      : temp(tmp) {}
   Definition(PhysReg reg, RegClass type) noexcept
      : temp(Temp(0, type))
   {
      setFixed(reg);
   }

   bool isTemp() const noexcept
   {
      return tempId() > 0;
   }

   Temp getTemp() noexcept
   {
      return temp;
   }

   uint32_t tempId() const noexcept
   {
      return temp.id();
   }

   void setTemp(Temp t) {
      temp = t;
   }

   RegClass regClass() const noexcept
   {
      return temp.regClass();
   }

   unsigned size() const noexcept
   {
      return temp.size();
   }

   bool isFixed() const noexcept
   {
      return control_[0];
   }

   PhysReg physReg() const noexcept
   {
      return reg_;
   }

   void setFixed(PhysReg reg) noexcept
   {
      control_[0] = 1;
      reg_ = reg;
   }

   bool mustReuseInput() const noexcept
   {
      return control_[1];
   }

   void setReuseInput(bool v) noexcept
   {
      control_[1] = v;
   }

   void setHint(PhysReg reg) noexcept
   {
      control_[2] = 1;
      reg_ = reg;
   }

   bool hasHint() const noexcept
   {
      return control_[2];
   }

private:
   Temp temp;
   std::bitset<8> control_;
   PhysReg reg_;
};

class Block;

struct Instruction {
   aco_opcode opcode;
   Format format;

   Operand *operands;
   uint32_t num_operands;

   Definition *definitions;
   uint32_t num_definitions;

   // TODO remove
   uint32_t definitionCount() const { return num_definitions; }
   uint32_t operandCount() const { return num_operands; }
   Operand& getOperand(int index) { return operands[index]; }
   Definition& getDefinition(int index) { return definitions[index]; }

   bool isVALU()
   {
      return ((uint16_t) format & (uint16_t) Format::VOP1) == (uint16_t) Format::VOP1
          || ((uint16_t) format & (uint16_t) Format::VOP2) == (uint16_t) Format::VOP2
          || ((uint16_t) format & (uint16_t) Format::VOPC) == (uint16_t) Format::VOPC
          || ((uint16_t) format & (uint16_t) Format::VOP3A) == (uint16_t) Format::VOP3A
          || ((uint16_t) format & (uint16_t) Format::VOP3B) == (uint16_t) Format::VOP3B
          || ((uint16_t) format & (uint16_t) Format::VOP3P) == (uint16_t) Format::VOP3P;
   }
   bool isSALU()
   {
      return format == Format::SOP1 ||
             format == Format::SOP2 ||
             format == Format::SOPC ||
             format == Format::SOPK ||
             format == Format::SOPP;
   }
   bool isVMEM()
   {
      return format == Format::MTBUF ||
             format == Format::MUBUF ||
             format == Format::MIMG;
   }
   bool isDPP()
   {
      return (uint16_t) format & (uint16_t) Format::DPP;
   }
   bool isVOP3()
   {
      return ((uint16_t) format & (uint16_t) Format::VOP3A) ||
             ((uint16_t) format & (uint16_t) Format::VOP3B) ||
             format == Format::VOP3P;
   }
};

struct SOPK_instruction : public Instruction {
   uint16_t imm;
};

struct SOPP_instruction : public Instruction {
   uint32_t imm;
   Block *block;
};

struct SOPC_instruction : public Instruction {
};

struct SOP1_instruction : public Instruction {
};

struct SOP2_instruction : public Instruction {
};

/**
 * Scalar Memory Format:
 * For s_(buffer_)load_dword*:
 * Operand(0): SBASE - SGPR-pair which provides base address
 * Operand(1): Offset - immediate (un)signed offset or SGPR
 * Operand(2): SOffset - SGPR offset (Vega only)
 * Definition(0): SDATA - SGPR which accepts return data
 *
 */
struct SMEM_instruction : public Instruction {
   bool glc; /* VI+: globally coherent */
   bool nv; /* VEGA only: Non-volatile */
};

struct VOP1_instruction : public Instruction {
};

struct VOP2_instruction : public Instruction {
};

struct VOPC_instruction : public Instruction {
};

struct VOP3A_instruction : public Instruction {
   bool abs[3];
   bool opsel[3];
   bool clamp;
   unsigned omod;
   bool neg[3];
};

/**
 * Data Parallel Primitives Format:
 * This format can be used for VOP1, VOP2 or VOPC instructions.
 * The swizzle applies to the src0 operand.
 *
 */
struct DPP_instruction : public Instruction {
   uint16_t dpp_ctrl;
   uint8_t row_mask;
   uint8_t bank_mask;
   bool abs[2];
   bool neg[2];
   bool bound_ctrl;
};

struct Interp_instruction : public Instruction {
   unsigned attribute;
   unsigned component;
};

/**
 * Local and Global Data Sharing instructions
 * Operand(0): ADDR - VGPR which supplies the address.
 * Operand(1): DATA0 - First data VGPR.
 * Operand(2): DATA1 - Second data VGPR.
 * Definition(0): VDST - Destination VGPR when results returned to VGPRs.
 *
 */
struct DS_instruction : public Instruction {
   int16_t offset0;
   int8_t offset1;
   bool gds;
};

/**
 * Vector Memory Untyped-buffer Instructions
 * Operand(0): VADDR - Address source. Can carry an index and/or offset
 * Operand(1): SRSRC - Specifies which SGPR supplies T# (resource constant)
 * Operand(2): SOFFSET - SGPR to supply unsigned byte offset. (SGPR, M0, or inline constant)
 * Operand(3) / Definition(0): VDATA - Vector GPR for write result / read data
 *
 */
struct MUBUF_instruction : public Instruction {
   unsigned dfmt; /* Data Format of data in memory buffer */
   unsigned nfmt; /* Numeric format of data in memory */
   unsigned offset; /* Unsigned byte offset - 12 bit */
   bool offen; /* Supply an offset from VGPR (VADDR) */
   bool idxen; /* Supply an index from VGPR (VADDR) */
   bool glc; /* globally coherent */
   bool slc; /* system level coherent */
   bool tfe; /* texture fail enable */
   bool lds; /* Return read-data to LDS instead of VGPRs */
};

/**
 * Vector Memory Image Instructions
 * Operand(0): VADDR - Address source. Can carry an offset or an index.
 * Operand(1): SRSRC - Scalar GPR that specifies the resource constant.
 * Operand(2): SSAMP - Scalar GPR that specifies sampler constant.
 * Definition(0): VDATA - Vector GPR for write result.
 *
 */
struct MIMG_instruction : public Instruction {
   unsigned dmask; /* Data VGPR enable mask */
   bool unrm; /* Force address to be un-normalized */
   bool glc; /* globally coherent */
   bool slc; /* system level coherent */
   bool tfe; /* texture fail enable */
   bool da; /* declare an array */
   bool lwe; /* Force data to be un-normalized */
   union {
      bool r128; /* Texture resource size */
      bool a16; /* VEGA: Address components are 16-bits */
   };
   bool d16; /* Convert 32-bit data to 16-bit data */
};

struct Export_instruction : public Instruction {
   unsigned enabled_mask;
   unsigned dest;
   bool compressed;
   bool done;
   bool valid_mask;

};

struct Pseudo_branch_instruction : public Instruction {
   Block *targets[2];
};

template<typename T>
T* create_instruction(aco_opcode opcode, Format format, uint32_t num_operands, uint32_t num_definitions)
{
   std::size_t size = sizeof(T) + num_operands * sizeof(Operand) + num_definitions * sizeof(Definition);
   char *data = (char*)calloc(1, size);
   auto inst = (T*)data;

   inst->opcode = opcode;
   inst->format = format;
   inst->num_operands = num_operands;
   inst->num_definitions = num_definitions;

   inst->operands = (Operand*)(data + sizeof(T));
   inst->definitions = (Definition*)(data + sizeof(T) + num_operands * sizeof(Operand));

   return inst;
}

/* CFG */
struct Block {
   unsigned index;
   std::vector<std::unique_ptr<Instruction>> instructions;
   std::vector<Block*> logical_predecessors;
   std::vector<Block*> linear_predecessors;
   std::vector<Block*> logical_successors;
   std::vector<Block*> linear_successors;
   unsigned vgpr_demand;
   unsigned sgpr_demand;
};


class Program final {
public:
   std::vector<std::unique_ptr<Block>> blocks;
   unsigned vgpr_demand = 0;
   unsigned sgpr_demand = 0;
   ac_shader_config* config;
   struct radv_shader_variant_info *info;
   enum chip_class chip_class;

   uint32_t allocateId()
   {
      return allocationID++;
   }

   uint32_t peekAllocationId()
   {
      return allocationID;
   }

   void setAllocationId(uint32_t id)
   {
      allocationID = id;
   }

   Block* createAndInsertBlock()
   {
      Block* b = new Block{(unsigned) blocks.size()};
      blocks.push_back(std::unique_ptr<Block>(b));
      return b;
   }

private:
   uint32_t allocationID = 1;
};

std::unique_ptr<Program> select_program(struct nir_shader *nir,
                                        ac_shader_config* config,
                                        struct radv_shader_variant_info *info,
                                        struct radv_nir_compiler_options *options);
std::vector<std::set<Temp>> live_temps_at_end_of_block(Program* program);
void value_numbering(Program* program);
void optimize(Program* program);
void register_allocation(Program *program);
void eliminate_phis(Program* program);
void lower_to_hw_instr(Program* program);
void schedule(Program* program);
void schedule_program(Program* program);
void insert_wait_states(Program* program);
std::vector<uint32_t> emit_program(Program* program);
void print_asm(std::vector<uint32_t>& binary, char* llvm_mc, std::ostream& out);
void validate(Program* program, FILE *output);

void aco_print_instr(Instruction *instr, FILE *output);
void aco_print_program(Program *program, FILE *output);

}
#endif /* __cplusplus */
#endif /* ACO_IR_H */
