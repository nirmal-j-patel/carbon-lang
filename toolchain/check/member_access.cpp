// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "toolchain/check/member_access.h"

#include <optional>

#include "llvm/ADT/STLExtras.h"
#include "toolchain/base/kind_switch.h"
#include "toolchain/check/context.h"
#include "toolchain/check/convert.h"
#include "toolchain/check/impl_lookup.h"
#include "toolchain/diagnostics/diagnostic_emitter.h"
#include "toolchain/sem_ir/generic.h"
#include "toolchain/sem_ir/ids.h"
#include "toolchain/sem_ir/inst.h"
#include "toolchain/sem_ir/name_scope.h"
#include "toolchain/sem_ir/typed_insts.h"

namespace Carbon::Check {

// Returns the index of the specified class element within the class's
// representation.
static auto GetClassElementIndex(Context& context, SemIR::InstId element_id)
    -> SemIR::ElementIndex {
  auto element_inst = context.insts().Get(element_id);
  if (auto field = element_inst.TryAs<SemIR::FieldDecl>()) {
    return field->index;
  }
  if (auto base = element_inst.TryAs<SemIR::BaseDecl>()) {
    return base->index;
  }
  CARBON_FATAL("Unexpected value {0} in class element name", element_inst);
}

// Returns whether `function_id` is an instance method, that is, whether it has
// an implicit `self` parameter.
static auto IsInstanceMethod(const SemIR::File& sem_ir,
                             SemIR::FunctionId function_id) -> bool {
  const auto& function = sem_ir.functions().Get(function_id);
  for (auto param_id :
       sem_ir.inst_blocks().GetOrEmpty(function.implicit_param_patterns_id)) {
    if (SemIR::Function::GetNameFromPatternId(sem_ir, param_id) ==
        SemIR::NameId::SelfValue) {
      return true;
    }
  }

  return false;
}

// Returns the highest allowed access. For example, if this returns `Protected`
// then only `Public` and `Protected` accesses are allowed--not `Private`.
static auto GetHighestAllowedAccess(Context& context, SemIR::LocId loc_id,
                                    SemIR::ConstantId name_scope_const_id)
    -> SemIR::AccessKind {
  auto [_, self_type_inst_id] = context.LookupUnqualifiedName(
      loc_id.node_id(), SemIR::NameId::SelfType, /*required=*/false);
  if (!self_type_inst_id.is_valid()) {
    return SemIR::AccessKind::Public;
  }

  // TODO: Support other types for `Self`.
  auto self_class_type =
      context.insts().TryGetAs<SemIR::ClassType>(self_type_inst_id);
  if (!self_class_type) {
    return SemIR::AccessKind::Public;
  }

  auto self_class_info = context.classes().Get(self_class_type->class_id);

  // TODO: Support other types.
  if (auto class_type = context.insts().TryGetAs<SemIR::ClassType>(
          context.constant_values().GetInstId(name_scope_const_id))) {
    auto class_info = context.classes().Get(class_type->class_id);

    if (self_class_info.self_type_id == class_info.self_type_id) {
      return SemIR::AccessKind::Private;
    }

    // If the `type_id` of `Self` does not match with the one we're currently
    // accessing, try checking if this class is of the parent type of `Self`.
    if (auto base_type_id = self_class_info.GetBaseType(
            context.sem_ir(), self_class_type->specific_id);
        base_type_id.is_valid()) {
      if (context.types().GetConstantId(base_type_id) == name_scope_const_id) {
        return SemIR::AccessKind::Protected;
      }
      // TODO: Also check whether this base class has a base class of its own.
    } else if (auto adapt_type_id = self_class_info.GetAdaptedType(
                   context.sem_ir(), self_class_type->specific_id);
               adapt_type_id.is_valid()) {
      if (context.types().GetConstantId(adapt_type_id) == name_scope_const_id) {
        // TODO: Should we be allowed to access protected fields of a type we
        // are adapting? The design doesn't allow this.
        return SemIR::AccessKind::Protected;
      }
    }
  }

  return SemIR::AccessKind::Public;
}

// Returns whether `scope` is a scope for which impl lookup should be performed
// if we find an associated entity.
static auto ScopeNeedsImplLookup(Context& context,
                                 SemIR::ConstantId name_scope_const_id)
    -> bool {
  SemIR::InstId inst_id =
      context.constant_values().GetInstId(name_scope_const_id);
  CARBON_CHECK(inst_id.is_valid());
  SemIR::Inst inst = context.insts().Get(inst_id);

  if (inst.Is<SemIR::FacetType>()) {
    // Don't perform impl lookup if an associated entity is named as a member of
    // a facet type.
    return false;
  }
  if (inst.Is<SemIR::Namespace>()) {
    // Don't perform impl lookup if an associated entity is named as a namespace
    // member.
    // TODO: This case is not yet listed in the design.
    return false;
  }
  // Any other kind of scope is assumed to be a type that implements the
  // interface containing the associated entity, and impl lookup is performed.
  return true;
}

static auto GetInterfaceFromFacetType(Context& context, SemIR::TypeId type_id)
    -> std::optional<SemIR::FacetTypeInfo::ImplsConstraint> {
  auto facet_type = context.types().GetAs<SemIR::FacetType>(type_id);
  const auto& facet_type_info =
      context.facet_types().Get(facet_type.facet_type_id);
  return facet_type_info.TryAsSingleInterface();
}

static auto AccessMemberOfInterfaceWitness(
    Context& context, SemIR::LocId loc_id, SemIR::InstId witness_id,
    SemIR::SpecificId interface_specific_id,
    SemIR::AssociatedEntityType assoc_type, SemIR::InstId member_id)
    -> SemIR::InstId {
  auto member_value_id = context.constant_values().GetConstantInstId(member_id);
  if (!member_value_id.is_valid()) {
    if (member_value_id != SemIR::InstId::BuiltinErrorInst) {
      context.TODO(member_id, "non-constant associated entity");
    }
    return SemIR::InstId::BuiltinErrorInst;
  }

  auto assoc_entity =
      context.insts().TryGetAs<SemIR::AssociatedEntity>(member_value_id);
  if (!assoc_entity) {
    context.TODO(member_id, "unexpected value for associated entity");
    return SemIR::InstId::BuiltinErrorInst;
  }

  // TODO: This produces the type of the associated entity with no value for
  // `Self`. The type `Self` might appear in the type of an associated constant,
  // and if so, we'll need to substitute it here somehow.
  auto subst_type_id = SemIR::GetTypeInSpecific(
      context.sem_ir(), interface_specific_id, assoc_type.entity_type_id);

  return context.GetOrAddInst<SemIR::InterfaceWitnessAccess>(
      loc_id, {.type_id = subst_type_id,
               .witness_id = witness_id,
               .index = assoc_entity->index});
}

// Performs impl lookup for a member name expression. This finds the relevant
// impl witness and extracts the corresponding impl member.
static auto PerformImplLookup(
    Context& context, SemIR::LocId loc_id, SemIR::ConstantId type_const_id,
    SemIR::AssociatedEntityType assoc_type, SemIR::InstId member_id,
    Context::BuildDiagnosticFn missing_impl_diagnoser = nullptr)
    -> SemIR::InstId {
  auto interface_type =
      GetInterfaceFromFacetType(context, assoc_type.interface_type_id);
  if (!interface_type) {
    context.TODO(loc_id,
                 "Lookup of impl witness not yet supported except for a single "
                 "interface");
    return SemIR::InstId::BuiltinErrorInst;
  }

  auto witness_id =
      LookupInterfaceWitness(context, loc_id, type_const_id,
                             assoc_type.interface_type_id.AsConstantId());
  if (!witness_id.is_valid()) {
    auto interface_type_id = context.GetInterfaceType(
        interface_type->interface_id, interface_type->specific_id);
    if (missing_impl_diagnoser) {
      // TODO: Pass in the expression whose type we are printing.
      CARBON_DIAGNOSTIC(MissingImplInMemberAccessNote, Note,
                        "type {1} does not implement interface {0}",
                        SemIR::TypeId, SemIR::TypeId);
      missing_impl_diagnoser()
          .Note(loc_id, MissingImplInMemberAccessNote, interface_type_id,
                context.GetTypeIdForTypeConstant(type_const_id))
          .Emit();
    } else {
      // TODO: Pass in the expression whose type we are printing.
      CARBON_DIAGNOSTIC(MissingImplInMemberAccess, Error,
                        "cannot access member of interface {0} in type {1} "
                        "that does not implement that interface",
                        SemIR::TypeId, SemIR::TypeId);
      context.emitter().Emit(loc_id, MissingImplInMemberAccess,
                             interface_type_id,
                             context.GetTypeIdForTypeConstant(type_const_id));
    }
    return SemIR::InstId::BuiltinErrorInst;
  }
  return AccessMemberOfInterfaceWitness(context, loc_id, witness_id,
                                        interface_type->specific_id, assoc_type,
                                        member_id);
}

// Performs a member name lookup into the specified scope, including performing
// impl lookup if necessary. If the scope is invalid, assume an error has
// already been diagnosed, and return BuiltinErrorInst.
static auto LookupMemberNameInScope(Context& context, SemIR::LocId loc_id,
                                    SemIR::InstId base_id,
                                    SemIR::NameId name_id,
                                    SemIR::ConstantId name_scope_const_id,
                                    llvm::ArrayRef<LookupScope> lookup_scopes,
                                    bool lookup_in_type_of_base)
    -> SemIR::InstId {
  AccessInfo access_info = {
      .constant_id = name_scope_const_id,
      .highest_allowed_access =
          GetHighestAllowedAccess(context, loc_id, name_scope_const_id),
  };
  LookupResult result =
      context.LookupQualifiedName(loc_id, name_id, lookup_scopes,
                                  /*required=*/true, access_info);

  if (!result.inst_id.is_valid()) {
    return SemIR::InstId::BuiltinErrorInst;
  }

  // TODO: This duplicates the work that HandleNameAsExpr does. Factor this out.
  auto inst = context.insts().Get(result.inst_id);
  auto type_id = SemIR::GetTypeInSpecific(context.sem_ir(), result.specific_id,
                                          inst.type_id());
  CARBON_CHECK(type_id.is_valid(), "Missing type for member {0}", inst);

  // If the named entity has a constant value that depends on its specific,
  // store the specific too.
  if (result.specific_id.is_valid() &&
      context.constant_values().Get(result.inst_id).is_symbolic()) {
    result.inst_id = context.GetOrAddInst<SemIR::SpecificConstant>(
        loc_id, {.type_id = type_id,
                 .inst_id = result.inst_id,
                 .specific_id = result.specific_id});
  }

  // TODO: Use a different kind of instruction that also references the
  // `base_id` so that `SemIR` consumers can find it.
  auto member_id = context.GetOrAddInst<SemIR::NameRef>(
      loc_id,
      {.type_id = type_id, .name_id = name_id, .value_id = result.inst_id});

  // If member name lookup finds an associated entity name, and the scope is not
  // a facet type, perform impl lookup.
  //
  // TODO: We need to do this as part of searching extended scopes, because a
  // lookup that finds an associated entity and also finds the corresponding
  // impl member is not supposed to be treated as ambiguous.
  if (auto assoc_type =
          context.types().TryGetAs<SemIR::AssociatedEntityType>(type_id)) {
    if (lookup_in_type_of_base) {
      SemIR::TypeId base_type_id = context.insts().Get(base_id).type_id();
      if (base_type_id != SemIR::TypeType::SingletonTypeId &&
          context.IsFacetType(base_type_id)) {
        // Handles `T.F` when `T` is a non-type facet.

        auto assoc_interface =
            GetInterfaceFromFacetType(context, assoc_type->interface_type_id);
        // An associated entity should always be associated with a single
        // interface.
        CARBON_CHECK(assoc_interface);

        // First look for `*assoc_interface` in the type of the base. If it is
        // found, get the witness that the interface is implemented from
        // `base_id`.
        auto facet_type = context.types().GetAs<SemIR::FacetType>(base_type_id);
        const auto& facet_type_info =
            context.facet_types().Get(facet_type.facet_type_id);
        // Witness that `T` implements the `*assoc_interface`.
        SemIR::InstId witness_inst_id = SemIR::InstId::Invalid;
        for (auto base_interface : facet_type_info.impls_constraints) {
          // Get the witness that `T` implements `base_type_id`.
          if (base_interface == *assoc_interface) {
            witness_inst_id = context.GetOrAddInst<SemIR::FacetAccessWitness>(
                loc_id, {.type_id = context.GetBuiltinType(
                             SemIR::BuiltinInstKind::WitnessType),
                         .facet_value_inst_id = base_id});
            // TODO: Result will eventually be a facet type witness instead of
            // an interface witness. Will need to use the index
            // `*assoc_interface` was found in
            // `facet_type_info.impls_constraints` to get the correct interface
            // witness out.
            break;
          }
        }
        // TODO: If that fails, would need to do impl lookup to see if the facet
        // value implements the interface of `*assoc_type`.
        if (!witness_inst_id.is_valid()) {
          context.TODO(member_id,
                       "associated entity not found in facet type, need to do "
                       "impl lookup");
          return SemIR::InstId::BuiltinErrorInst;
        }

        member_id = AccessMemberOfInterfaceWitness(
            context, loc_id, witness_inst_id, assoc_interface->specific_id,
            *assoc_type, member_id);
      } else {
        // Handles `x.F` if `x` is of type `class C` that extends an interface
        // containing `F`.
        SemIR::ConstantId constant_id =
            context.types().GetConstantId(base_type_id);
        member_id = PerformImplLookup(context, loc_id, constant_id, *assoc_type,
                                      member_id);
      }
    } else if (ScopeNeedsImplLookup(context, name_scope_const_id)) {
      // Handles `T.F` where `T` is a type extending an interface containing
      // `F`.
      member_id = PerformImplLookup(context, loc_id, name_scope_const_id,
                                    *assoc_type, member_id);
    }
  }

  return member_id;
}

// Performs the instance binding step in member access. If the found member is a
// field, forms a class member access. If the found member is an instance
// method, forms a bound method. Otherwise, the member is returned unchanged.
static auto PerformInstanceBinding(Context& context, SemIR::LocId loc_id,
                                   SemIR::InstId base_id,
                                   SemIR::InstId member_id) -> SemIR::InstId {
  auto member_type_id = context.insts().Get(member_id).type_id();
  CARBON_KIND_SWITCH(context.types().GetAsInst(member_type_id)) {
    case CARBON_KIND(SemIR::UnboundElementType unbound_element_type): {
      // Convert the base to the type of the element if necessary.
      base_id = ConvertToValueOrRefOfType(context, loc_id, base_id,
                                          unbound_element_type.class_type_id);

      // Find the specified element, which could be either a field or a base
      // class, and build an element access expression.
      auto element_id = context.constant_values().GetConstantInstId(member_id);
      CARBON_CHECK(element_id.is_valid(),
                   "Non-constant value {0} of unbound element type",
                   context.insts().Get(member_id));
      auto index = GetClassElementIndex(context, element_id);
      auto access_id = context.GetOrAddInst<SemIR::ClassElementAccess>(
          loc_id, {.type_id = unbound_element_type.element_type_id,
                   .base_id = base_id,
                   .index = index});
      if (SemIR::GetExprCategory(context.sem_ir(), base_id) ==
              SemIR::ExprCategory::Value &&
          SemIR::GetExprCategory(context.sem_ir(), access_id) !=
              SemIR::ExprCategory::Value) {
        // Class element access on a value expression produces an ephemeral
        // reference if the class's value representation is a pointer to the
        // object representation. Add a value binding in that case so that the
        // expression category of the result matches the expression category of
        // the base.
        access_id = ConvertToValueExpr(context, access_id);
      }
      return access_id;
    }
    case CARBON_KIND(SemIR::FunctionType fn_type): {
      if (IsInstanceMethod(context.sem_ir(), fn_type.function_id)) {
        return context.GetOrAddInst<SemIR::BoundMethod>(
            loc_id, {.type_id = context.GetBuiltinType(
                         SemIR::BuiltinInstKind::BoundMethodType),
                     .object_id = base_id,
                     .function_id = member_id});
      }
      [[fallthrough]];
    }
    default:
      // Not an instance member: no instance binding.
      return member_id;
  }
}

// Validates that the index (required to be an IntValue) is valid within the
// tuple size. Returns the index on success, or nullptr on failure.
static auto ValidateTupleIndex(Context& context, SemIR::LocId loc_id,
                               SemIR::InstId operand_inst_id,
                               SemIR::IntValue index_inst, int size)
    -> std::optional<llvm::APInt> {
  llvm::APInt index_val = context.ints().Get(index_inst.int_id);
  if (index_val.uge(size)) {
    CARBON_DIAGNOSTIC(TupleIndexOutOfBounds, Error,
                      "tuple element index `{0}` is past the end of type {1}",
                      TypedInt, TypeOfInstId);
    context.emitter().Emit(loc_id, TupleIndexOutOfBounds,
                           {.type = index_inst.type_id, .value = index_val},
                           operand_inst_id);
    return std::nullopt;
  }
  return index_val;
}

auto PerformMemberAccess(Context& context, SemIR::LocId loc_id,
                         SemIR::InstId base_id, SemIR::NameId name_id)
    -> SemIR::InstId {
  // If the base is a name scope, such as a class or namespace, perform lookup
  // into that scope.
  if (auto base_const_id = context.constant_values().Get(base_id);
      base_const_id.is_constant()) {
    llvm::SmallVector<LookupScope> lookup_scopes;
    if (context.AppendLookupScopesForConstant(loc_id, base_const_id,
                                              &lookup_scopes)) {
      return LookupMemberNameInScope(context, loc_id, base_id, name_id,
                                     base_const_id, lookup_scopes,
                                     /*lookup_in_type_of_base=*/false);
    }
  }

  // If the base isn't a scope, it must have a complete type.
  auto base_type_id = context.insts().Get(base_id).type_id();
  if (!context.TryToCompleteType(base_type_id, [&] {
        CARBON_DIAGNOSTIC(IncompleteTypeInMemberAccess, Error,
                          "member access into object of incomplete type {0}",
                          TypeOfInstId);
        return context.emitter().Build(base_id, IncompleteTypeInMemberAccess,
                                       base_id);
      })) {
    return SemIR::InstId::BuiltinErrorInst;
  }

  // Materialize a temporary for the base expression if necessary.
  base_id = ConvertToValueOrRefExpr(context, base_id);
  base_type_id = context.insts().Get(base_id).type_id();
  auto base_type_const_id = context.types().GetConstantId(base_type_id);

  // Find the scope corresponding to the base type.
  llvm::SmallVector<LookupScope> lookup_scopes;
  if (!context.AppendLookupScopesForConstant(loc_id, base_type_const_id,
                                             &lookup_scopes)) {
    // The base type is not a name scope. Try some fallback options.
    if (auto struct_type = context.insts().TryGetAs<SemIR::StructType>(
            context.constant_values().GetInstId(base_type_const_id))) {
      // TODO: Do we need to optimize this with a lookup table for O(1)?
      for (auto [i, field] : llvm::enumerate(
               context.struct_type_fields().Get(struct_type->fields_id))) {
        if (name_id == field.name_id) {
          // TODO: Model this as producing a lookup result, and do instance
          // binding separately. Perhaps a struct type should be a name scope.
          return context.GetOrAddInst<SemIR::StructAccess>(
              loc_id, {.type_id = field.type_id,
                       .struct_id = base_id,
                       .index = SemIR::ElementIndex(i)});
        }
      }
      CARBON_DIAGNOSTIC(QualifiedExprNameNotFound, Error,
                        "type {0} does not have a member `{1}`", TypeOfInstId,
                        SemIR::NameId);
      context.emitter().Emit(loc_id, QualifiedExprNameNotFound, base_id,
                             name_id);
      return SemIR::InstId::BuiltinErrorInst;
    }

    if (base_type_id != SemIR::ErrorInst::SingletonTypeId) {
      CARBON_DIAGNOSTIC(QualifiedExprUnsupported, Error,
                        "type {0} does not support qualified expressions",
                        TypeOfInstId);
      context.emitter().Emit(loc_id, QualifiedExprUnsupported, base_id);
    }
    return SemIR::InstId::BuiltinErrorInst;
  }

  // Perform lookup into the base type.
  auto member_id = LookupMemberNameInScope(context, loc_id, base_id, name_id,
                                           base_type_const_id, lookup_scopes,
                                           /*lookup_in_type_of_base=*/true);

  // Perform instance binding if we found an instance member.
  member_id = PerformInstanceBinding(context, loc_id, base_id, member_id);

  return member_id;
}

auto PerformCompoundMemberAccess(
    Context& context, SemIR::LocId loc_id, SemIR::InstId base_id,
    SemIR::InstId member_expr_id,
    Context::BuildDiagnosticFn missing_impl_diagnoser) -> SemIR::InstId {
  auto base_type_id = context.insts().Get(base_id).type_id();
  auto base_type_const_id = context.types().GetConstantId(base_type_id);

  auto member_id = member_expr_id;
  auto member = context.insts().Get(member_id);

  // If the member expression names an associated entity, impl lookup is always
  // performed using the type of the base expression.
  if (auto assoc_type = context.types().TryGetAs<SemIR::AssociatedEntityType>(
          member.type_id())) {
    member_id =
        PerformImplLookup(context, loc_id, base_type_const_id, *assoc_type,
                          member_id, missing_impl_diagnoser);
  } else if (context.insts().Is<SemIR::TupleType>(
                 context.constant_values().GetInstId(base_type_const_id))) {
    return PerformTupleAccess(context, loc_id, base_id, member_expr_id);
  }

  // Perform instance binding if we found an instance member.
  member_id = PerformInstanceBinding(context, loc_id, base_id, member_id);

  // If we didn't perform impl lookup or instance binding, that's an error
  // because the base expression is not used for anything.
  if (member_id == member_expr_id &&
      member.type_id() != SemIR::ErrorInst::SingletonTypeId) {
    CARBON_DIAGNOSTIC(CompoundMemberAccessDoesNotUseBase, Error,
                      "member name of type {0} in compound member access is "
                      "not an instance member or an interface member",
                      TypeOfInstId);
    context.emitter().Emit(loc_id, CompoundMemberAccessDoesNotUseBase,
                           member_id);
  }

  return member_id;
}

auto PerformTupleAccess(Context& context, SemIR::LocId loc_id,
                        SemIR::InstId tuple_inst_id,
                        SemIR::InstId index_inst_id) -> SemIR::InstId {
  tuple_inst_id = ConvertToValueOrRefExpr(context, tuple_inst_id);
  auto tuple_type_id = context.insts().Get(tuple_inst_id).type_id();

  auto tuple_type = context.types().TryGetAs<SemIR::TupleType>(tuple_type_id);
  if (!tuple_type) {
    CARBON_DIAGNOSTIC(TupleIndexOnANonTupleType, Error,
                      "type {0} does not support tuple indexing; only "
                      "tuples can be indexed that way",
                      TypeOfInstId);
    context.emitter().Emit(loc_id, TupleIndexOnANonTupleType, tuple_inst_id);
    return SemIR::InstId::BuiltinErrorInst;
  }

  auto diag_non_constant_index = [&] {
    // TODO: Decide what to do if the index is a symbolic constant.
    CARBON_DIAGNOSTIC(TupleIndexNotConstant, Error,
                      "tuple index must be a constant");
    context.emitter().Emit(loc_id, TupleIndexNotConstant);
    return SemIR::InstId::BuiltinErrorInst;
  };
  // Diagnose a non-constant index prior to conversion to IntLiteral, because
  // the conversion will fail if the index is not constant.
  if (!context.constant_values().Get(index_inst_id).is_template()) {
    return diag_non_constant_index();
  }

  SemIR::TypeId element_type_id = SemIR::ErrorInst::SingletonTypeId;
  auto index_node_id = context.insts().GetLocId(index_inst_id);
  index_inst_id = ConvertToValueOfType(
      context, index_node_id, index_inst_id,
      context.GetBuiltinType(SemIR::BuiltinInstKind::IntLiteralType));
  auto index_const_id = context.constant_values().Get(index_inst_id);
  if (index_const_id == SemIR::ErrorInst::SingletonConstantId) {
    return SemIR::InstId::BuiltinErrorInst;
  } else if (!index_const_id.is_template()) {
    return diag_non_constant_index();
  }

  auto index_literal = context.insts().GetAs<SemIR::IntValue>(
      context.constant_values().GetInstId(index_const_id));
  auto type_block = context.type_blocks().Get(tuple_type->elements_id);
  std::optional<llvm::APInt> index_val = ValidateTupleIndex(
      context, loc_id, tuple_inst_id, index_literal, type_block.size());
  if (!index_val) {
    return SemIR::InstId::BuiltinErrorInst;
  }

  // TODO: Handle the case when `index_val->getZExtValue()` has too many bits.
  element_type_id = type_block[index_val->getZExtValue()];
  auto tuple_index = SemIR::ElementIndex(index_val->getZExtValue());

  return context.GetOrAddInst<SemIR::TupleAccess>(loc_id,
                                                  {.type_id = element_type_id,
                                                   .tuple_id = tuple_inst_id,
                                                   .index = tuple_index});
}

}  // namespace Carbon::Check
