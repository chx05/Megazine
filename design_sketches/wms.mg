OrderCode u32
Order
  code OrderCode #key
  topick Content
  picked Content
  planned_by_device_id ?DeviceID

TotalQty u32
Qty u16
Content Dict[ItemCode, Qty]

LockableQty
  free_qty Qty
  locked_qty Qty

ItemCode u32
Item
  code ItemCode #key
  description str
  locations OrderedDict[FatCoordinate, LockableQty]
  available_qty_for_new_orders TotalQty

Sector enum
  picking
  stocking

Coordinate
  sector Sector
  aisle u16
  column u16
  shelf u16
  bin u16
SortingN u32
FatCoordinate
  coord Coordinate #key
  sorting_n SortingN #ordering_key

DeviceID u16
Device
  id DeviceID #key
  logged_user ?Username
  picking_plan ?PickingPlan

Username str #min(1) #max(20)
User
  username Username #key
  password str #min(3) #max(20)

PickingLog
  id u64 #key
  device_id DeviceID
  source_coordinate Coordinate
  item_code ItemCode
  order_code OrderCode
  qty Qty

Logs
  picking_logs Tab[PickingLog]

db
  orders Tab[Order]
  items Tab[Item]
  devices Tab[Device]
  users Tab[User]

  sorting_numbers Tab[FatCoordinate]
  logs Logs


OrderCodeAlreadyInUse void
OrderRequestNotFullfillable
  item_code ItemCode
  requested_qty Qty
  available_qty TotalQty

ImportOrderResult union
  void
  OrderCodeAlreadyInUse #err
  OrderRequestNotFullfillable #err

[mut]
import_order(code OrderCode, request Content) ImportOrderResult
  | code already in use?
  if code in db.orders
    raise OrderCodeAlreadyInUse()

  | can fulfill order?
  | and decrease item's available qty if order is imported
  for item_code, requested_qty in request
    item = db.items.get(item_code) else
      raise OrderRequestNotFullfillable(item_code, requested_qty, available_qty: 0)

    defer db.items.set(item)

    available_qty = item.available_qty_for_new_orders
    if requested_qty > available_qty
      raise OrderRequestNotFullfillable(item_code, requested_qty, available_qty)

    item.available_qty_for_new_orders -= requested_qty
  

  | insert order
  db.orders.set(Order(
    code,
    topick: request,
    picked: Content()
  ))


ContentPerOrder Dict[ItemCode, Dict[OrderCode, Qty]]
PickingPlan OrderedDict[FatCoordinate, ContentPerOrder]
DeviceHasSomethingAlreadyPlanned void
OrderAlreadyPlanned
  order_code OrderCode
CouldNotPlanAllOrders
  order_codes Set[OrderCode]
  item_codes Set[ItemCode]

PlanPickingResult union
  PickingPlan
  DeviceHasSomethingAlreadyPlanned #err
  OrderAlreadyPlanned #err
  CouldNotPlanAllOrders #err
  GetAllErr[OrderCode] #err
  GetErr #err

[mut]
plan_picking(
  order_codes Set[OrderCode],
  device_id DeviceID,
  allowed_sectors ?Set[Sector] = null
) PlanPickingResult

  | verify device
  device = db.devices.get(device_id).try
  defer db.devices.set(device)
  if device.picking_plan != null
    raise DeviceHasSomethingAlreadyPlanned()
  
  | load all orders
  | ensures none of them is already planned
  | flags each one as planned
  orders = db.orders.get_all(order_codes).try
  block
    for order in orders
      if order.planned_by_device_id != null
        raise OrderAlreadyPlanned(order_code: order.code)
      
      order.planned_by_device_id = device_id

    | i don't use defer here because i need to temporarely modify each order
    | later but i don't want to commit any of that modification,
    | it's just for intermediate calculations
    db.orders.set_all(orders)

  | union all orders.topick into a single big topick list
  topick = orders.map(o -> o.topick).distinct_flatten_by_sum()

  | build a temporary OrderedDict[FatCoordinate, Dict[ItemCode, Qty]] for all items in topick, ignoring non allowed sectors
  | this dict only contains involved coordinates, not every single warehouse location
  location_contents = OrderedDict[FatCoordinate, Content]()
  for item_code, qty in topick
    item = db.items.get(item_code).unwrap
    for fat_coord, lockable_qty in item.locations
      if allowed_sectors != null and fat_coord.coord.sector not in allowed_sectors.unwrap
        continue

      location_contents.add_if_not_there(fat_coord, Content())
      | item_code can't already be inside here (item.locations has no dupplicate keys, nor topick)
      location_contents[fat_coord].add(item_code, lockable_qty.free_qty).unwrap

  | build the final picking plan by decreasing step after step quantities from each involved order
  | this step also locks quantities in the chosen coordinates
  plan = PickingPlan()
  block
    | for each collected coordinate, we consume what it offers
    | based on provided orders
    | some content of the 
    for fat_coord, content in location_contents
      content_per_order = ContentPerOrder()
      for item_code, available_qty in content
        per_order_qty = Dict[OrderCode, Qty]()
        content_per_order.add(item_code, per_order_qty).unwrap

        item = db.items.get(item_code).unwrap
        defer db.items.set(item)

        while available_qty > 0
          order = orders.first(o -> item_code in o.topick) else
            break
          
          qty_to_pick = min(available_qty, order.topick[item_code])
          per_order_qty.add(order.code, qty_to_pick).unwrap

          item.locations[fat_coord].free_qty -= qty_to_pick
          item.locations[fat_coord].locked_qty += qty_to_pick

          available_qty -= qty_to_pick
          order.topick[item_code] -= qty_to_pick
          if order.topick[item_code] == 0
            order.topick.remove(item_code)

            if order.topick.empty()
              orders.remove(order)

        | let's break immediately so we don't iterate for nothing
        if orders.empty()
          break

      if not content_per_order.empty()
        plan.add(fat_coord, content_per_order).unwrap
      | re-check here as well, let's not iterate remaining coordinates if we already fulfilled all orders
      if orders.empty()
        break
        
  
  if not orders.empty()
    raise CouldNotPlanAllOrders(
      order_codes: orders.map(o -> o.code),
      item_codes: orders.map(o -> o.topick.keys()).distinct_flatten()
    )

  | associate the generated plan to the device  
  device.picking_plan = plan

  return plan


| Todo
|modify_picking_plan(plan PickingPlan) PickingPlan
|  pass


| Todo
|unplan_picking(device_id DeviceID)
|  pass


DeviceHasNothingPlanned void
CoordinateNotInPlan void
ExcludedUnplannedItem
  item_code ItemCode
ExcludedTooMuchQty
  item_code ItemCode
  order_code OrderCode
  qty Qty
ExcludedUnplannedOrder
  item_code ItemCode
  order_code OrderCode
  qty Qty

PickCoordinateResult union
  void
  CoordinateNotInPlan #err
  DeviceHasNothingPlanned #err
  ExcludedUnplannedItem #err
  ExcludedTooMuchQty #err
  ExcludedUnplannedOrder #err

| Todo
| track progress of picked locations in device.picking_plan
[mut]
pick_coordinate(
  source_coordinate Coordinate,
  device_id DeviceID,
  exclusions ContentPerOrder = ContentPerOrder()
) PickCoordinateResult

  | plan has source_coordinate? plan_step = plan[source_coordinate]
  device = db.devices.get(device_id).try
  if device.picking_plan == null
    raise DeviceHasNothingPlanned()
  plan = device.picking_plan.unwrap
  plan_step = plan.first(fatcoord -> fatcoord.coord == source_coordinate) else raise CoordinateNotInPlan()

  | apply exclusions to plan_step (an item might be partially picked instead or not picked at all)
  for item_code, excluded_per_order_qty in exclusions
    planned_per_order_qty = plan_step.get(item_code) else raise ExcludedUnplannedItem(item_code)
    
    for order_code, qty in excluded_per_order_qty
      planned_order_qty_ref = planned_per_order_qty.get_ref(order_code) else
        raise ExcludedUnplannedOrder(item_code, order_code, qty)
      
      if qty > *planned_order_qty_ref
        raise ExcludedTooMuchQty(item_code, order_code, qty)

      *planned_order_qty_ref -= qty
      
      if *planned_order_qty_ref == 0
        planned_per_order_qty.remove(order_code)

  | 1. decrease source coordinate content according to the refined plan_step
  | 2. move from topick to picked for each order of the refined plan_step
  | 3. log movement
  for item_code, per_order_qty in plan_step
    total_qty_to_pick_for_current_item = per_order_qty.sum((order_code, qty) -> qty)

    | decrease from source (only locked qty, because this was planned)
    item = db.items.get(item_code).unwrap
    defer db.items.set(item)
    *item.locations.ref_first((fatcoord, value) -> fatcoord.coord == source_coordinate).locked_qty -= total_qty_to_pick_for_current_item
    
    | 1. move to picked for each order involved
    | 2. log movement
    for order_code, qty in per_order_qty
      | decrease topick (remove item if 0 remaining), increase picked
      block
        order = db.orders.get(order_code).unwrap
        defer db.orders.set(order)

        topick_ref = &order.topick[item_code]
        *topick_ref -= qty

        if *topick_ref == 0
          order.topick.remove(item_code)
        
        order.picked.add_if_not_there(item_code, 0)
        order.picked[item_code] += qty

      | log picking movement
      db.logs.picking_logs.set(PickingLog(
        id: db.logs.picking_logs.next_id(),
        device_id,
        source_coordinate,
        item_code,
        order_code,
        qty
      ))
